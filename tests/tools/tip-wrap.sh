#!/bin/sh
real=${TIP_REAL:-/usr/local/bin/tundra-ip}
if [ -n "${TIP_FAIL:-}" ] && [ -e "$TIP_FAIL" ] && [ "$1 $2" = "route replace" ]; then
	echo "tundra-ip: injected failure" >&2
	exit "$(cat "$TIP_FAIL")"
fi
if [ -n "${TIP_FAIL:-}" ] && [ -e "$TIP_FAIL.mac" ] && [ "$1 $2 $3" = "link set mac" ]; then
	echo "tundra-ip: injected failure" >&2
	exit 1
fi
exec "$real" "$@"
