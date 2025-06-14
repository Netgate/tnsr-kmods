#!/bin/bash

for a in /sys/class/leds/led8500*; do
  echo 0 > $a/brightness
  echo none > $a/trigger
done

for c in amber blue red green; do
  for a in /sys/class/leds/led8500\:$c\:*; do
    echo 1 > $a/brightness
  done
  sleep 1.5
done

for c in amber blue red green; do
  for a in /sys/class/leds/led8500\:$c\:*; do
    echo timer > $a/trigger
  done
  sleep 3
done

for a in /sys/class/leds/led8500*; do
  echo 0 > $a/brightness
done
