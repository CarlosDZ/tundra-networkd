#!/bin/sh
exec "${WPA_REAL:-/usr/bin/wpa_supplicant}" "$@"
