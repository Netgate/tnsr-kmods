// SPDX-License-Identifier: GPL-2.0
/*
 * Netgate 5200 / 7200 (Silicom "Ibiza" platform) status LED and reset button
 * driver.
 *
 * The LEDs and the recessed reset button are handled by the MEC1723
 * embedded controller.  The BIOS exposes them through the ACPI device
 * SILC1003 (\_SB.PC00.LPCB.H_EC.SLCM) with these methods:
 *
 *   LDSO(idx)          host takes ownership of LED idx (one-way, repeatable)
 *   LDCS(idx, 0xRRGGBB) set the colour of RGB LED idx
 *   LDBL(idx, val)     set the brightness of LED idx, 0 = off; RGB LEDs
 *                      take 0..255, amber LEDs 1..100 (larger values are
 *                      ignored).  Any LDBL call also stops a hardware blink.
 *   LDBS(idx, (off_ms << 16) | on_ms)  hardware blink, 0 = stop blinking
 *   RSTB(val)          reset button soft mode: 1 = the host handles it
 *   RSTG()             read the soft mode flag
 *
 * Button press and release are reported as ACPI notifications 0xC5 and
 * 0xC6 on the same device.
 *
 * Each of these methods stores its arguments in EC registers shared by
 * all LED commands, and then waits for the EC command register (CMDR,
 * EC address 0x3a) to become free before writing the command.  The
 * arguments are thus written while the previous command may still be
 * pending, and calls issued back to back from the kernel do corrupt
 * each other (observed as blink settings being lost).  So after each
 * call the driver waits for CMDR to clear before issuing the next one.
 *
 * LED indices, as seen on the front panel (LEDs are named by position):
 *
 *   0  RGB   bottom (circle on the 5200 faceplate); the BIOS makes it solid
 *            blue at ACPI init
 *   1  RGB   middle (square)
 *   2  RGB   top (diamond)
 *   3  amber top      (each amber LED shares the light pipe with the RGB
 *   4  amber middle    LED at the same position)
 *   5  amber bottom
 *
 * The reset button is exposed as input line 0 ("RESET_BUTTON") of the
 * gpiochip labelled "ibiza-ec", with edge events.  Like the reset button
 * GPIO on the 8300/8500 the line is active low (0 while pressed), so the
 * same gpiomon based monitor (gpiomon --active-low) works unchanged.
 *
 * Copyright (C) 2026 Rubicon Communications, LLC (Netgate)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/dmi.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define IBIZA_NOTIFY_BUTTON_PRESS	0xc5
#define IBIZA_NOTIFY_BUTTON_RELEASE	0xc6

#define IBIZA_BLINK_DEFAULT_MS		500
#define IBIZA_BLINK_MAX_MS		0xffff

#define IBIZA_AMBER_ON			100

#define IBIZA_EC_CMDR			0x3a
#define IBIZA_EC_POLL_US		200
#define IBIZA_EC_TIMEOUT_US		(500 * USEC_PER_MSEC)

#define IBIZA_GPIO_LABEL		"ibiza-ec"

struct ibiza_priv;

struct ibiza_rgb_led {
	struct led_classdev_mc		mc_cdev;
	struct mc_subled		subled[3];
	struct ibiza_priv		*priv;
	int				index;
	/*
	 * Hardware blink parameter while blinking, 0 otherwise.  The EC
	 * stops blinking on every brightness change, so it is re-sent after
	 * each one.  Protected by ec_lock.
	 */
	u32				blink;
};

struct ibiza_amber_led {
	struct led_classdev		cdev;
	struct ibiza_priv		*priv;
	int				index;
};

struct ibiza_priv {
	struct device			*dev;
	acpi_handle			handle;
	/* Serializes the EC method calls (each is a multi-step sequence). */
	struct mutex			ec_lock;

	struct ibiza_rgb_led		rgb[3];
	struct ibiza_amber_led		amber[3];

	struct gpio_chip		gc;
	/* Serializes button state changes (ACPI notify vs. debugfs). */
	struct mutex			button_lock;
	bool				pressed;
	bool				irq_enabled;
	unsigned int			irq_type;

	unsigned long long		saved_rst_mode;
	bool				rst_mode_saved;
	bool				notify_installed;
	struct dentry			*debugfs_dir;
};

/*
 * Other Ibiza based systems may use the same ACPI device, but their LED
 * layout and reset button handling have not been verified, so the driver
 * binds only on the systems below unless told otherwise.
 */
static bool ignore_dmi;
module_param(ignore_dmi, bool, 0444);
MODULE_PARM_DESC(ignore_dmi, "Bind even if the system is not a Netgate 5200 or 7200");

static const struct dmi_system_id ibiza_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Netgate"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "5200"),
		},
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Netgate"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "7200"),
		},
	},
	{ }
};

static const struct {
	const char *name;
	int index;
} ibiza_rgb_leds[] = {
	{ "ibiza:multicolor:bottom",	0 },
	{ "ibiza:multicolor:middle",	1 },
	{ "ibiza:multicolor:top",	2 },
};

static const struct {
	const char *name;
	int index;
} ibiza_amber_leds[] = {
	{ "ibiza:amber:top",		3 },
	{ "ibiza:amber:middle",		4 },
	{ "ibiza:amber:bottom",		5 },
};

/*
 * EC method helpers
 */

static int ibiza_ec_cmdr(void)
{
	u8 val;
	int ret;

	ret = ec_read(IBIZA_EC_CMDR, &val);
	if (ret)
		return ret;

	return val;
}

/* Wait until the EC has taken the last command. */
static int ibiza_ec_wait_idle(struct ibiza_priv *priv)
{
	int ret, cmdr;

	ret = read_poll_timeout(ibiza_ec_cmdr, cmdr, cmdr <= 0,
				IBIZA_EC_POLL_US, IBIZA_EC_TIMEOUT_US, false);
	if (ret) {
		dev_warn_ratelimited(priv->dev,
				     "EC command 0x%02x still pending\n", cmdr);
		return ret;
	}
	if (cmdr < 0) {
		dev_err_ratelimited(priv->dev, "EC read failed: %d\n", cmdr);
		return cmdr;
	}

	return 0;
}

static int ibiza_call(struct ibiza_priv *priv, const char *method,
		      int nargs, u64 arg0, u64 arg1)
{
	union acpi_object args[2] = {
		{ .integer = { .type = ACPI_TYPE_INTEGER, .value = arg0 } },
		{ .integer = { .type = ACPI_TYPE_INTEGER, .value = arg1 } },
	};
	struct acpi_object_list arg_list = {
		.count = nargs,
		.pointer = args,
	};
	acpi_status status;

	lockdep_assert_held(&priv->ec_lock);

	status = acpi_evaluate_object(priv->handle, (acpi_string)method,
				      &arg_list, NULL);
	if (ACPI_FAILURE(status)) {
		dev_err_ratelimited(priv->dev, "%s(0x%llx, 0x%llx) failed: %s\n",
				    method, arg0, arg1,
				    acpi_format_exception(status));
		return -EIO;
	}

	return ibiza_ec_wait_idle(priv);
}

static int ibiza_call1(struct ibiza_priv *priv, const char *method, u64 arg)
{
	return ibiza_call(priv, method, 1, arg, 0);
}

static int ibiza_call2(struct ibiza_priv *priv, const char *method,
		       u64 arg0, u64 arg1)
{
	return ibiza_call(priv, method, 2, arg0, arg1);
}

/*
 * RGB LEDs
 */

static struct ibiza_rgb_led *cdev_to_rgb(struct led_classdev *cdev)
{
	return container_of(lcdev_to_mccdev(cdev), struct ibiza_rgb_led,
			    mc_cdev);
}

static u32 ibiza_rgb_color(struct ibiza_rgb_led *led)
{
	struct mc_subled *s = led->mc_cdev.subled_info;

	return (min_t(u32, s[0].intensity, 0xff) << 16) |
	       (min_t(u32, s[1].intensity, 0xff) << 8) |
	       min_t(u32, s[2].intensity, 0xff);
}

/* Caller holds ec_lock. */
static int ibiza_rgb_apply(struct ibiza_rgb_led *led,
			   enum led_brightness brightness)
{
	struct ibiza_priv *priv = led->priv;
	int ret;

	if (brightness == LED_OFF) {
		/* Stop a hardware blink too, so "off" really means off. */
		led->blink = 0;
		ret = ibiza_call2(priv, "LDBS", led->index, 0);
		if (ret)
			return ret;
		return ibiza_call2(priv, "LDBL", led->index, 0);
	}

	ret = ibiza_call2(priv, "LDCS", led->index, ibiza_rgb_color(led));
	if (ret)
		return ret;

	ret = ibiza_call2(priv, "LDBL", led->index, brightness);
	if (ret)
		return ret;

	/*
	 * LDBL stops a hardware blink.  Keep blinking, as the LED core
	 * expects: sysfs brightness writes are applied from a work item, so
	 * they commonly arrive after the timer trigger has set up blinking.
	 */
	if (led->blink)
		ret = ibiza_call2(priv, "LDBS", led->index, led->blink);

	return ret;
}

static int ibiza_rgb_brightness_set(struct led_classdev *cdev,
				    enum led_brightness brightness)
{
	struct ibiza_rgb_led *led = cdev_to_rgb(cdev);
	int ret;

	mutex_lock(&led->priv->ec_lock);
	ret = ibiza_rgb_apply(led, brightness);
	mutex_unlock(&led->priv->ec_lock);

	return ret;
}

static int ibiza_rgb_blink_set(struct led_classdev *cdev,
			       unsigned long *delay_on,
			       unsigned long *delay_off)
{
	struct ibiza_rgb_led *led = cdev_to_rgb(cdev);
	struct ibiza_priv *priv = led->priv;
	enum led_brightness brightness;
	int ret;

	if (*delay_on == 0 && *delay_off == 0) {
		*delay_on = IBIZA_BLINK_DEFAULT_MS;
		*delay_off = IBIZA_BLINK_DEFAULT_MS;
	}
	*delay_on = min_t(unsigned long, *delay_on, IBIZA_BLINK_MAX_MS);
	*delay_off = min_t(unsigned long, *delay_off, IBIZA_BLINK_MAX_MS);

	/*
	 * Brightness changes from sysfs are applied asynchronously from a
	 * work item.  Let the pending ones complete first, otherwise a
	 * "brightness 0" queued just before the trigger was set (writing
	 * multi_intensity while the LED is off does that, too) would arrive
	 * later and stop the blinking.  blink_set() may also run from that
	 * work item (led_blink_set_nosleep()), so don't wait for ourselves.
	 */
	if (current_work() != &cdev->set_brightness_work)
		flush_work(&cdev->set_brightness_work);

	/* Like the software timer trigger: blink at full brightness if off. */
	brightness = cdev->brightness ? cdev->brightness : cdev->max_brightness;

	mutex_lock(&priv->ec_lock);
	led->blink = (*delay_off << 16) | *delay_on;
	ret = ibiza_rgb_apply(led, brightness);
	mutex_unlock(&priv->ec_lock);

	return ret;
}

static int ibiza_register_rgb(struct ibiza_priv *priv, int i)
{
	struct ibiza_rgb_led *led = &priv->rgb[i];
	struct led_classdev *cdev = &led->mc_cdev.led_cdev;

	led->priv = priv;
	led->index = ibiza_rgb_leds[i].index;

	led->subled[0].color_index = LED_COLOR_ID_RED;
	led->subled[1].color_index = LED_COLOR_ID_GREEN;
	led->subled[2].color_index = LED_COLOR_ID_BLUE;
	led->mc_cdev.subled_info = led->subled;
	led->mc_cdev.num_colors = ARRAY_SIZE(led->subled);

	cdev->name = ibiza_rgb_leds[i].name;
	cdev->max_brightness = LED_FULL;
	cdev->brightness_set_blocking = ibiza_rgb_brightness_set;
	cdev->blink_set = ibiza_rgb_blink_set;

	return devm_led_classdev_multicolor_register(priv->dev, &led->mc_cdev);
}

/*
 * Amber LEDs: on/off only, no hardware blink (the timer trigger falls
 * back to software blinking).
 */

static int ibiza_amber_brightness_set(struct led_classdev *cdev,
				      enum led_brightness brightness)
{
	struct ibiza_amber_led *led =
		container_of(cdev, struct ibiza_amber_led, cdev);
	int ret;

	mutex_lock(&led->priv->ec_lock);
	ret = ibiza_call2(led->priv, "LDBL", led->index,
			  brightness ? IBIZA_AMBER_ON : 0);
	mutex_unlock(&led->priv->ec_lock);

	return ret;
}

static int ibiza_register_amber(struct ibiza_priv *priv, int i)
{
	struct ibiza_amber_led *led = &priv->amber[i];

	led->priv = priv;
	led->index = ibiza_amber_leds[i].index;
	led->cdev.name = ibiza_amber_leds[i].name;
	led->cdev.max_brightness = 1;
	led->cdev.brightness_set_blocking = ibiza_amber_brightness_set;

	return devm_led_classdev_register(priv->dev, &led->cdev);
}

static void ibiza_all_leds_off(struct ibiza_priv *priv)
{
	int i;

	mutex_lock(&priv->ec_lock);
	for (i = 0; i < ARRAY_SIZE(ibiza_rgb_leds); i++)
		ibiza_rgb_apply(&priv->rgb[i], LED_OFF);
	for (i = 0; i < ARRAY_SIZE(ibiza_amber_leds); i++)
		ibiza_call2(priv, "LDBL", ibiza_amber_leds[i].index, 0);
	mutex_unlock(&priv->ec_lock);
}

/*
 * Reset button as a GPIO line with edge interrupts.
 *
 * There is no hardware interrupt behind the line: the ACPI notify
 * handler (process context) updates the cached state and then runs the
 * nested interrupt handler of the line.  gpiolib-cdev reads the line
 * value to tell rising from falling edges, so get() returns the cached
 * state, which always matches the event being delivered.
 *
 * The line is active low: its raw value is 0 while the button is pressed,
 * so a press is a falling edge and a release a rising edge.
 */

static int ibiza_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_IN;
}

static int ibiza_gpio_direction_input(struct gpio_chip *gc,
				      unsigned int offset)
{
	return 0;
}

static int ibiza_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ibiza_priv *priv = gpiochip_get_data(gc);

	return !READ_ONCE(priv->pressed);
}

static void ibiza_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ibiza_priv *priv = gpiochip_get_data(gc);

	WRITE_ONCE(priv->irq_enabled, false);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
#endif
}

static void ibiza_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ibiza_priv *priv = gpiochip_get_data(gc);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
#endif
	WRITE_ONCE(priv->irq_enabled, true);
}

static int ibiza_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ibiza_priv *priv = gpiochip_get_data(gc);

	if (type & ~IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;

	WRITE_ONCE(priv->irq_type, type);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static const struct irq_chip ibiza_irq_chip = {
	.name = IBIZA_GPIO_LABEL,
	.irq_mask = ibiza_irq_mask,
	.irq_unmask = ibiza_irq_unmask,
	.irq_set_type = ibiza_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};
#else
static struct irq_chip ibiza_irq_chip = {
	.name = IBIZA_GPIO_LABEL,
	.irq_mask = ibiza_irq_mask,
	.irq_unmask = ibiza_irq_unmask,
	.irq_set_type = ibiza_irq_set_type,
	.flags = IRQCHIP_SKIP_SET_WAKE,
};
#endif

static const char * const ibiza_gpio_names[] = { "RESET_BUTTON" };

static void ibiza_button_event(struct ibiza_priv *priv, bool pressed)
{
	unsigned int type, irq;

	mutex_lock(&priv->button_lock);

	if (priv->pressed == pressed) {
		dev_dbg(priv->dev, "duplicate button %s event ignored\n",
			pressed ? "press" : "release");
		goto out;
	}
	WRITE_ONCE(priv->pressed, pressed);
	dev_dbg(priv->dev, "reset button %s\n",
		pressed ? "pressed" : "released");

	if (!READ_ONCE(priv->irq_enabled))
		goto out;

	type = READ_ONCE(priv->irq_type);
	if (!(type & (pressed ? IRQ_TYPE_EDGE_FALLING : IRQ_TYPE_EDGE_RISING)))
		goto out;

	irq = irq_find_mapping(priv->gc.irq.domain, 0);
	if (irq)
		handle_nested_irq(irq);
out:
	mutex_unlock(&priv->button_lock);
}

static void ibiza_acpi_notify(acpi_handle handle, u32 event, void *context)
{
	struct ibiza_priv *priv = context;

	switch (event) {
	case IBIZA_NOTIFY_BUTTON_PRESS:
		ibiza_button_event(priv, true);
		break;
	case IBIZA_NOTIFY_BUTTON_RELEASE:
		ibiza_button_event(priv, false);
		break;
	default:
		dev_info(priv->dev, "unknown ACPI notification 0x%x\n", event);
		break;
	}
}

static int ibiza_register_gpio(struct ibiza_priv *priv)
{
	struct gpio_chip *gc = &priv->gc;
	struct gpio_irq_chip *girq = &gc->irq;

	gc->label = IBIZA_GPIO_LABEL;
	gc->parent = priv->dev;
	gc->owner = THIS_MODULE;
	gc->base = -1;
	gc->ngpio = ARRAY_SIZE(ibiza_gpio_names);
	gc->names = ibiza_gpio_names;
	gc->can_sleep = true;
	gc->get_direction = ibiza_gpio_get_direction;
	gc->direction_input = ibiza_gpio_direction_input;
	gc->get = ibiza_gpio_get;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	gpio_irq_chip_set_chip(girq, &ibiza_irq_chip);
#else
	girq->chip = &ibiza_irq_chip;
#endif
	girq->handler = handle_simple_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->threaded = true;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->parent_handler = NULL;

	return devm_gpiochip_add_data(priv->dev, gc, priv);
}

/*
 * debugfs: <debugfs>/ibiza_leds/button, write 1 (press) or 0 (release)
 * to simulate the button without anyone at the box.
 */

static ssize_t ibiza_debugfs_button_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct ibiza_priv *priv = file->private_data;
	bool pressed;
	int ret;

	ret = kstrtobool_from_user(buf, count, &pressed);
	if (ret)
		return ret;

	ibiza_button_event(priv, pressed);

	return count;
}

static ssize_t ibiza_debugfs_button_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct ibiza_priv *priv = file->private_data;
	char s[3];

	s[0] = READ_ONCE(priv->pressed) ? '1' : '0';
	s[1] = '\n';
	s[2] = '\0';

	return simple_read_from_buffer(buf, count, ppos, s, 2);
}

static const struct file_operations ibiza_debugfs_button_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ibiza_debugfs_button_read,
	.write = ibiza_debugfs_button_write,
	.llseek = default_llseek,
};

/*
 * Probe / remove
 */

static const char * const ibiza_required_methods[] = {
	"LDSO", "LDCS", "LDBL", "LDBS", "RSTB", "RSTG",
};

static void ibiza_cleanup(struct ibiza_priv *priv)
{
	debugfs_remove_recursive(priv->debugfs_dir);
	priv->debugfs_dir = NULL;

	if (priv->notify_installed) {
		acpi_remove_notify_handler(priv->handle, ACPI_DEVICE_NOTIFY,
					   ibiza_acpi_notify);
		priv->notify_installed = false;
	}

	if (priv->rst_mode_saved) {
		mutex_lock(&priv->ec_lock);
		ibiza_call1(priv, "RSTB", priv->saved_rst_mode);
		mutex_unlock(&priv->ec_lock);
		priv->rst_mode_saved = false;
	}
}

static int ibiza_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ibiza_priv *priv;
	acpi_status status;
	int i, ret;

	if (!dmi_check_system(ibiza_dmi_table) && !ignore_dmi) {
		dev_info(dev, "not a Netgate 5200/7200, not binding (see ignore_dmi)\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->handle = ACPI_HANDLE(dev);
	if (!priv->handle)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(ibiza_required_methods); i++) {
		if (!acpi_has_method(priv->handle,
				     (char *)ibiza_required_methods[i]))
			return dev_err_probe(dev, -ENODEV,
					     "ACPI method %s is missing\n",
					     ibiza_required_methods[i]);
	}

	mutex_init(&priv->ec_lock);
	mutex_init(&priv->button_lock);
	platform_set_drvdata(pdev, priv);

	/*
	 * Take ownership of the LEDs we use.  The EC ignores host writes to
	 * LEDs it owns; there is no way to hand them back, but the EC takes
	 * them back by itself across a reboot.
	 */
	mutex_lock(&priv->ec_lock);
	for (i = 0; i < ARRAY_SIZE(ibiza_rgb_leds); i++)
		ibiza_call1(priv, "LDSO", ibiza_rgb_leds[i].index);
	for (i = 0; i < ARRAY_SIZE(ibiza_amber_leds); i++)
		ibiza_call1(priv, "LDSO", ibiza_amber_leds[i].index);
	mutex_unlock(&priv->ec_lock);

	for (i = 0; i < ARRAY_SIZE(ibiza_rgb_leds); i++) {
		ret = ibiza_register_rgb(priv, i);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register %s\n",
					     ibiza_rgb_leds[i].name);
	}

	for (i = 0; i < ARRAY_SIZE(ibiza_amber_leds); i++) {
		ret = ibiza_register_amber(priv, i);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register %s\n",
					     ibiza_amber_leds[i].name);
	}

	ret = ibiza_register_gpio(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register gpiochip\n");

	/* Hand the reset button to the host, remembering the old mode. */
	status = acpi_evaluate_integer(priv->handle, "RSTG", NULL,
				       &priv->saved_rst_mode);
	if (ACPI_FAILURE(status))
		return dev_err_probe(dev, -EIO, "RSTG failed: %s\n",
				     acpi_format_exception(status));
	priv->rst_mode_saved = true;

	mutex_lock(&priv->ec_lock);
	ret = ibiza_call1(priv, "RSTB", 1);
	mutex_unlock(&priv->ec_lock);
	if (ret)
		goto err;

	status = acpi_install_notify_handler(priv->handle, ACPI_DEVICE_NOTIFY,
					     ibiza_acpi_notify, priv);
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
		dev_err(dev, "failed to install ACPI notify handler: %s\n",
			acpi_format_exception(status));
		goto err;
	}
	priv->notify_installed = true;

	priv->debugfs_dir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	debugfs_create_file("button", 0600, priv->debugfs_dir, priv,
			    &ibiza_debugfs_button_fops);

	dev_info(dev, "Netgate %s LEDs and reset button (reset mode was %llu)\n",
		 dmi_get_system_info(DMI_PRODUCT_NAME) ?: "Ibiza",
		 priv->saved_rst_mode);

	return 0;

err:
	ibiza_cleanup(priv);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void ibiza_remove(struct platform_device *pdev)
#else
static int ibiza_remove(struct platform_device *pdev)
#endif
{
	struct ibiza_priv *priv = platform_get_drvdata(pdev);

	ibiza_cleanup(priv);
	ibiza_all_leds_off(priv);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static const struct acpi_device_id ibiza_acpi_ids[] = {
	{ "SILC1003", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, ibiza_acpi_ids);

static struct platform_driver ibiza_driver = {
	.driver = {
		.name = "ibiza_leds",
		.acpi_match_table = ibiza_acpi_ids,
	},
	.probe = ibiza_probe,
	.remove = ibiza_remove,
};

module_platform_driver(ibiza_driver);

MODULE_AUTHOR("Rubicon Communications, LLC");
MODULE_DESCRIPTION("Netgate 5200/7200 (Ibiza) status LED and reset button driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
