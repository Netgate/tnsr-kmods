# TNSR kernel modules

This repository contains TNSR kernel modules built using DKMS
and some helper programs.

## Seville 8500 LED driver

Based on [8300 FreeBSD LED driver](https://gitlab.netgate.com/pfSense/FreeBSD-src/-/blob/5b32b00617de1f75f5ad557995c5184285eac220/sys/dev/netgate/led_8300.c).

Loading the module after the `deb` package is installed:
```console
# make
# modprobe 8500_leds
# modprobe ledtrig-timer
# echo "led8500 0x58" | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
```

Removing the module:
```console
# echo "0x58" | sudo tee /sys/bus/i2c/devices/i2c-1/delete_device
# rmmod 8500_leds ledtrig-timer
```

Available LEDs and their colors:
```console
# ls -d /sys/class/leds/led8500\:*
/sys/class/leds/led8500:amber:0  /sys/class/leds/led8500:green:0
/sys/class/leds/led8500:amber:1  /sys/class/leds/led8500:green:1
/sys/class/leds/led8500:amber:2  /sys/class/leds/led8500:green:2
/sys/class/leds/led8500:amber:3  /sys/class/leds/led8500:green:3
/sys/class/leds/led8500:blue:1   /sys/class/leds/led8500:red:1
/sys/class/leds/led8500:blue:2   /sys/class/leds/led8500:red:2
/sys/class/leds/led8500:blue:3   /sys/class/leds/led8500:red:3
```

Turning a LED on:
```console
# echo 1 > /sys/class/leds/led8500:amber:0/brightness
```

The command above will also automatically set other colors' brightness for the same LED,
namely `/sys/class/leds/led8500:{red,green,blue}:0/brightness` to `0`.

Turning a LED off:
```console
# echo 0 > /sys/class/leds/led8500:amber:0/brightness
```

Making a LED blink:
```console
# echo 1 > /sys/class/leds/led8500:blue:2/brightness
# echo timer > /sys/class/leds/led8500:blue:2/trigger
```

Stop blinking and turn off:
```console
# echo none > /sys/class/leds/led8500:blue:2/trigger
# echo 0 > /sys/class/leds/led8500:blue:2/brightness
```

## LED simulator

There's a simple LED simulator provided that prints which LEDs are
turned on and which are turned off, maintaining mutually exclusive LED
colors. The simulator does support blinking, too.


```console
# modprobe uleds
# modprobe ledtrig-timer
# make -C sim run
```

After that, you can trigger LEDs and make them blink as described in
the previous section regarding Seville 8500, and the corresponding
changes to LED status will be printed to stdout by the simulator
program.

The simulator can also be used for different LED setups, too:

```console
# sim/ledsim myled:green:{0,1} myled:blue:{0,1}
```
