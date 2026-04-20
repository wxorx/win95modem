#!/bin/sh
touch main/ppp_server_main.c && idf.py build && idf.py -p /dev/ttyACM1 flash && idf.py -p /dev/ttyACM1 monitor
