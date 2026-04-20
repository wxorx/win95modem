#!/bin/sh
touch main/ppp_server_main.c && idf.py build && idf.py -p /dev/tty.usbmodem142301 flash && idf.py -p /dev/tty.usbmodem142301 monitor
