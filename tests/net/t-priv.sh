. "$(dirname "$0")/lib.sh"

status_is() {
	grep -q "^$2:[[:space:]]*$3\$" "/proc/$1/status"
}

setup
fake run
cable_profile casa
start_daemon
expect_log "dhcpc\[$IF/casa\]: fake running" 5 "a child starts"
pid=$(child_pid casa)
check "the child is told who its parent is" \
	sh -c "tr '\0' '\n' <'/proc/$pid/environ' | grep -qx 'TUNDRA_PARENT_PID=$DAEMON_PID'"
stop_daemon

# Not root, with the capabilities the unit gives: bits 10, 12 and 13.
setup
uid=$(stat -c %u "$ROOT")
mkdir -p "$(dirname "$SOCK")" "$LEASES"
owner=$(stat -c %u:%g "$LEASES")
chown -R "$uid" "$(dirname "$SOCK")" "$RUN" "$CONF"
chown "$uid" "$LEASES"
: >"$LOG"
caps=+net_admin,+net_raw,+net_bind_service
ip netns exec "$NS" setpriv --reuid "$uid" --clear-groups \
	--inh-caps "$caps" --ambient-caps "$caps" "$NETWORKD" >>"$LOG" 2>&1 &
DAEMON_PID=$!
expect_log "control socket on $SOCK" 5 "the daemon runs without being root"
check "with nothing it can use" status_is "$DAEMON_PID" CapEff 0000000000000000
check "nor get back" status_is "$DAEMON_PID" CapPrm 0000000000000000
check "no ambient capability reaches every child" status_is "$DAEMON_PID" CapAmb 0000000000000000
check "only the inheritable ones of its children" status_is "$DAEMON_PID" CapInh 0000000000003400
stop_daemon
expect_exit 0 "and stops cleanly"
chown "$owner" "$LEASES"
summary
