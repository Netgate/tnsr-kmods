#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/version.h>

#define LED8500_DEVICE_ID_NON_PFR	0xe1
#define LED8500_DEVICE_ID_PFR		0xe2
#define LED8500_DEVICE_ID_RECOVERY	0xe3
#define LED8500_I2C_ADDR		0x58
#define LED8500_REG_DEVICE_ID		0x00
#define LED8500_REG_VERSION		0x01
#define LED8500_REG_BUILD_LOWER		0x02
#define LED8500_REG_BUILD_UPPER		0x03
#define LED8500_REG_RSTBTN		0x05
#define LED8500_SOURCE_SW		0x01
#define LED8500_VAL_OFF			0x00
#define LED8500_VAL_ON			0x01
#define LED8500_VAL_BLINK		0x03
#define LED8500_NUM_LEDS		4


struct led_8500_softc;
struct led_8500_multicolor;

struct led_8500 {
	struct led_classdev		led_cdev;
	struct led_8500_multicolor	*parent;
	u8				color;
	enum led_brightness		brightness;
	bool				blink_enabled;
};

struct led_8500_multicolor {
	struct led_8500_softc		*sc;
	u8				reg_color;
	u8				reg_source;
	u8				reg_val;
	struct led_8500			colors[4];
	int				num_colors;
};

struct led_8500_softc {
	struct i2c_client		*client;
	struct led_8500_multicolor	leds[4];
	struct mutex			lock;
};

static int led_8500_read_reg(struct led_8500_softc *sc, u8 reg, u8 *val)
{
	int ret;

	mutex_lock(&sc->lock);
	ret = i2c_smbus_read_byte_data(sc->client, reg);
	mutex_unlock(&sc->lock);

	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int led_8500_write_reg(struct led_8500_softc *sc, u8 reg, u8 val)
{
	int ret;

	mutex_lock(&sc->lock);
	ret = i2c_smbus_write_byte_data(sc->client, reg, val);
	mutex_unlock(&sc->lock);

	return ret;
}

static void led_8500_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct led_8500 *led = container_of(led_cdev, struct led_8500, led_cdev);
	u8 val = LED8500_VAL_OFF;
	int ret;

	if (brightness == 0)
		led->blink_enabled = false;

	led->brightness = brightness;

	if (brightness != LED_OFF)
		val = led->blink_enabled ? LED8500_VAL_BLINK : LED8500_VAL_ON;

	ret = led_8500_write_reg(led->parent->sc, led->parent->reg_source, LED8500_SOURCE_SW);
	if (ret < 0) {
		dev_err(&led->parent->sc->client->dev,
			"Failed to set source register: %d\n", ret);
		return;
	}

	ret = led_8500_write_reg(led->parent->sc, led->parent->reg_color, led->color);
	if (ret < 0) {
		dev_err(&led->parent->sc->client->dev,
			"Failed to set color register: %d\n", ret);
		return;
	}

	ret = led_8500_write_reg(led->parent->sc, led->parent->reg_val, val);
	if (ret < 0) {
		dev_err(&led->parent->sc->client->dev,
			"Failed to set value register: %d\n", ret);
	}
}

static int led_8500_blink_set(struct led_classdev *led_cdev,
			      unsigned long *delay_on,
			      unsigned long *delay_off)
{
	struct led_8500 *led = container_of(led_cdev, struct led_8500, led_cdev);

	/* Hardware blink is enabled, but we don't control the timing */
	led->blink_enabled = true;
	*delay_on = 500;
	*delay_off = 500;

	led_8500_brightness_set(led_cdev, led->brightness);

	return 0;
}

static int led_8500_create_led(struct led_8500_softc *sc, int led_idx, int color_idx,
			       const char *color_name, u8 color_val)
{
	struct led_8500 *led = &sc->leds[led_idx].colors[color_idx];
	char name[32];
	int ret;

	snprintf(name, sizeof(name), "led8500:%s:%d", color_name, led_idx);

	led->parent = &sc->leds[led_idx];
	led->color = color_val;
	led->brightness = LED_OFF;
	led->blink_enabled = false;

	led->led_cdev.name = devm_kstrdup(&sc->client->dev, name, GFP_KERNEL);
	if (!led->led_cdev.name)
		return -ENOMEM;

	led->led_cdev.brightness_set = led_8500_brightness_set;
	led->led_cdev.blink_set = led_8500_blink_set;
	led->led_cdev.brightness = LED_OFF;
	led->led_cdev.max_brightness = LED_FULL;
	led->led_cdev.flags = LED_HW_PLUGGABLE;

	ret = devm_led_classdev_register(&sc->client->dev, &led->led_cdev);
	if (ret < 0) {
		dev_err(&sc->client->dev,
			"Failed to register LED %s: %d\n", name, ret);
		return ret;
	}

	return 0;
}

static int led_8500_create_multicolor(struct led_8500_softc *sc, int idx)
{
	int ret;

	switch (idx) {
	case 0:
		sc->leds[idx].reg_color = 0x1c;
		sc->leds[idx].reg_source = 0x1d;
		sc->leds[idx].reg_val = 0x1e;
		break;
	case 1:
		sc->leds[idx].reg_color = 0x10;
		sc->leds[idx].reg_source = 0x11;
		sc->leds[idx].reg_val = 0x12;
		break;
	case 2:
		sc->leds[idx].reg_color = 0x14;
		sc->leds[idx].reg_source = 0x15;
		sc->leds[idx].reg_val = 0x16;
		break;
	case 3:
		sc->leds[idx].reg_color = 0x18;
		sc->leds[idx].reg_source = 0x19;
		sc->leds[idx].reg_val = 0x1a;
		break;
	default:
		return -EINVAL;
	}

	sc->leds[idx].sc = sc;

	if (idx == 0) {
		/* The power LED only supports green and amber */
		ret = led_8500_create_led(sc, idx, 0, "green", 0);
		if (ret)
			return ret;

		ret = led_8500_create_led(sc, idx, 1, "amber", 1);
		if (ret)
			return ret;

		sc->leds[idx].num_colors = 2;
	} else {
		/* For each of R/G/B/Amber */
		ret = led_8500_create_led(sc, idx, 0, "red", 0);
		if (ret)
			return ret;

		ret = led_8500_create_led(sc, idx, 1, "blue", 1);
		if (ret)
			return ret;

		ret = led_8500_create_led(sc, idx, 2, "green", 2);
		if (ret)
			return ret;

		ret = led_8500_create_led(sc, idx, 3, "amber", 3);
		if (ret)
			return ret;

		sc->leds[idx].num_colors = 4;
	}

	return 0;
}

static void led_8500_take_rstbtn(struct led_8500_softc *sc)
{
	int ret;

	ret = led_8500_write_reg(sc, LED8500_REG_RSTBTN, 1);
	if (ret < 0) {
		dev_warn(&sc->client->dev,
			 "Failed to take reset button control: %d\n", ret);
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int led_8500_probe(struct i2c_client *client)
#else
static int led_8500_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
#endif
{
	struct led_8500_softc *sc;
	u8 device_id, ver, b_low, b_up;
	int ret, i;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "SMBus byte data not supported\n");
		return -EIO;
	}

	sc = devm_kzalloc(&client->dev, sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->client = client;
	mutex_init(&sc->lock);
	i2c_set_clientdata(client, sc);

	/* Verify device ID */
	ret = led_8500_read_reg(sc, LED8500_REG_DEVICE_ID, &device_id);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to read device ID: %d\n", ret);
		return ret;
	}

	if (device_id != LED8500_DEVICE_ID_NON_PFR &&
	    device_id != LED8500_DEVICE_ID_PFR &&
	    device_id != LED8500_DEVICE_ID_RECOVERY) {
		dev_err(&client->dev, "Invalid device ID: 0x%02x\n", device_id);
		return -ENODEV;
	}

	/* Create LED devices */
	for (i = 0; i < LED8500_NUM_LEDS; i++) {
		ret = led_8500_create_multicolor(sc, i);
		if (ret) {
			dev_err(&client->dev,
				"Failed to create LED group %d: %d\n", i, ret);
			return ret;
		}
	}

	/* Take control of reset button */
	led_8500_take_rstbtn(sc);

	/* Read version information */
	led_8500_read_reg(sc, LED8500_REG_VERSION, &ver);
	led_8500_read_reg(sc, LED8500_REG_BUILD_LOWER, &b_low);
	led_8500_read_reg(sc, LED8500_REG_BUILD_UPPER, &b_up);

	dev_info(&client->dev,
		 "8500 FPGA, version %d, build %02x%02x\n",
		 ver, b_up, b_low);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void led_8500_remove(struct i2c_client *client)
#else
static int led_8500_remove(struct i2c_client *client)
#endif
{
	int i, j;
	struct led_8500_softc *sc = i2c_get_clientdata(client);

	/* Turn off all LEDs */
	for (i = 0; i < LED8500_NUM_LEDS; i++) {
		for (j = 0; j < sc->leds[i].num_colors; j++) {
			led_set_brightness(&sc->leds[i].colors[j].led_cdev, LED_OFF);
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct i2c_device_id led_8500_id[] = {
	{ "led8500", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, led_8500_id);

static const struct of_device_id led_8500_of_match[] = {
	{ .compatible = "led8500" },
	{ }
};
MODULE_DEVICE_TABLE(of, led_8500_of_match);

static struct i2c_driver led_8500_driver = {
	.driver = {
		.name = "led8500",
		.of_match_table = led_8500_of_match,
	},
	.probe = led_8500_probe,
	.remove = led_8500_remove,
	.id_table = led_8500_id,
};

module_i2c_driver(led_8500_driver);

MODULE_AUTHOR("Rubicon Communications, LLC");
MODULE_DESCRIPTION("8500 Seville I2C LED Controller Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
