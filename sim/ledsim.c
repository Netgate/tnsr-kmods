/*
 * ledsim: monitor userspace LEDs created with the uleds driver
 *         and keep colours of the same BASE:INDEX mutually exclusive.
 *
 * Build:
 *      cc -Wall -O2 ledsim.c -o ledsim
 *
 * Example usage:
 *      sudo ./ledsim led8500:{amber:{0..3},red:{1..3},blue:{1..3},green:{0..3}}
 *
 * Whenever (say) led8500:amber:0 is set to 1, the monitor writes 0 to
 * led8500:{green,red,blue}:0 automatically, and so on.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/uleds.h>

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif


static int sysfs_set_brightness(const char *led_name, int value)
{
	char path[256];
	snprintf(path, sizeof(path),
	         "/sys/class/leds/%s/brightness", led_name);

	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd == -1)
		return -1;

	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%d\n", value);
	int rc  = write(fd, buf, len) == len ? 0 : -1;
	close(fd);
	return rc;
}


struct led_key {
	char base[LED_MAX_NAME_SIZE];
	char colour[LED_MAX_NAME_SIZE];
	int  index; /* -1 on parse error */
};


/*
 * Parse BASE:COLOR:NUMBER string
 */
static int parse_led_name(const char *name, struct led_key *key)
{
	const char *last = strrchr(name, ':');
	if (!last)
		return -1;
	const char *mid = memrchr(name, ':', last - name);
	if (!mid)
		return -1;

	size_t len_base   = mid  - name;
	size_t len_colour = last - mid - 1;
	if (len_base >= sizeof key->base || len_colour >= sizeof key->colour)
		return -1;

	memcpy(key->base,   name,         len_base);   key->base[len_base]   = '\0';
	memcpy(key->colour, mid + 1,      len_colour); key->colour[len_colour] = '\0';
	key->index = atoi(last + 1);
	return 0;
}

struct led_ctx {
	int          fd;                      /* /dev/uleds FD                    */
	char         name[LED_MAX_NAME_SIZE];
	struct led_key key;                   /* parsed BASE/COLOUR/INDEX         */
	int          brightness;              /* our cached value                 */
};

static int register_led(struct led_ctx *ctx, const char *name,
                        unsigned int max_brightness)
{
	struct uleds_user_dev dev = { .max_brightness = max_brightness };
	strncpy(dev.name, name, LED_MAX_NAME_SIZE-1);
	dev.name[LED_MAX_NAME_SIZE-1] = 0;

	ctx->fd = open("/dev/uleds", O_RDWR | O_CLOEXEC);
	if (ctx->fd == -1) {
		perror("open /dev/uleds");
		return -1;
	}
	if (write(ctx->fd, &dev, sizeof(dev)) != sizeof(dev)) {
		perror("write /dev/uleds");
		close(ctx->fd);
		return -1;
	}

	strncpy(ctx->name, name, sizeof ctx->name);
	ctx->brightness = 0;

	if (parse_led_name(name, &ctx->key)) {
		fprintf(stderr, "Bad LED name '%s' (need BASE:COLOUR:NUM)\n", name);
		close(ctx->fd);
		return -1;
	}

	return 0;
}


/* Are two LEDs siblings (same BASE & INDEX but different colour)? */
static int is_sibling(const struct led_key *a, const struct led_key *b)
{
	return a->index == b->index &&
	       strcmp(a->base,   b->base) == 0 &&
	       strcmp(a->colour, b->colour) != 0;
}


int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s BASE:COLOUR:NUM [ ... ]\n", argv[0]);
		return EXIT_FAILURE;
	}
	const int nleds = argc - 1;

	struct led_ctx *leds = calloc(nleds, sizeof *leds);
	struct pollfd  *pfds = calloc(nleds, sizeof *pfds);
	if (!leds || !pfds) {
		perror("calloc");
		return EXIT_FAILURE;
	}

	/* Create /dev/uleds instances for every LED the user passed */
	for (int i = 0; i < nleds; ++i) {
		if (register_led(&leds[i], argv[i + 1], 255)) {
			fprintf(stderr, "Failed to set up LED '%s'\n", argv[i + 1]);
			return EXIT_FAILURE;
		}
		pfds[i].fd     = leds[i].fd;
		pfds[i].events = POLLIN;
	}

	printf("Monitoring %d LED%s…\n", nleds, nleds == 1 ? "" : "s");

	for (;;) {
		if (poll(pfds, nleds, -1) == -1) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		for (int i = 0; i < nleds; ++i) {
			if (!(pfds[i].revents & POLLIN))
				continue;

			int brightness, n;
			if ((n = read(pfds[i].fd, &brightness, sizeof brightness)) < 0) {
				perror("read");
				continue;
			}
			if (n != sizeof brightness) {
				fprintf(stderr, "bad read count %d", n);
				continue;
			}

			if (brightness == leds[i].brightness)
				continue; /* no change */

			leds[i].brightness = brightness;

			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			printf("[%ld.%09ld] %-32s %3d\n",
			       ts.tv_sec, ts.tv_nsec, leds[i].name, brightness);
			fflush(stdout);

			if (brightness == 0)
				continue; /* the LED is off */

			/* If this LED just turned on, switch off its siblings */
			for (int j = 0; j < nleds; ++j) {
				if (j == i)
					continue;
				if (!is_sibling(&leds[i].key, &leds[j].key))
					continue;
				if (leds[j].brightness == 0)
					continue;

				if (sysfs_set_brightness(leds[j].name, 0) == -1)
					perror("write brightness");
			}
		}
	}

	for (int i = 0; i < nleds; ++i)
		close(leds[i].fd);
	free(pfds);
	free(leds);
	return EXIT_SUCCESS;
}
