# tundra-networkd

Network orchestrator, developed for Tundra OS. It connects the networks described
in the `.tnc` profiles of `/etc/tundra/network/`, over cable and WiFi, supervises
`tundra-dhcpc`, `wpa_supplicant` and the `tundra-blueice` sensors, applies fixed
addresses, writes `resolv.conf` and publishes state and alerts on a control
socket. It never decides on its own: it
runs what the profiles and the user ask for.

## Requirements

- Linux.
- Root, or the `tundra-network` user with the capabilities of the unit (see
  Privileges).
- `tundra-ip` 1.8_0, `tundra-dhcpc` 2.1_0, `tundra-blueice-arp` 0.2_0 and
  `wpa_supplicant` 2.12 or later, at the paths fixed at build time
  (`TUNDRA_IP_PATH`, `TUNDRA_DHCPC_PATH`, `TUNDRA_BLUEICE_ARP_PATH`,
  `TUNDRA_WPA_SUPPLICANT_PATH`, under `/usr/bin` by default). The Tundra build of
  `wpa_supplicant` follows ADR-028.
- A `network` group for the users allowed on the control socket.

## Usage

```
systemctl enable --now tundra-networkd
systemctl reload tundra-networkd
tundra-networkd --version
```

The unit is installed disabled. `reload` rereads the configuration (`SIGHUP`).
Stopping the daemon leaves every address in place, so a restart does not cut
the network.

## Configuration

| Path | Purpose |
|------|---------|
| `/etc/tundra/network/*.tnc` | Profiles, read in lexicographic order |
| `/etc/tundra/network/interfaces` | Per interface properties |
| `/etc/tundra/network/000-template.tnc` | Commented template with every key, never loaded |
| `/etc/tundra/network/999-dhcp.tnc` | Fallback: DHCP on any wired interface |
| `/etc/tundra/network/route_tables` | Table names, read by `tundra-ip` only |

A profile has a `match` section (`interface`, and `ssid` and `security` for
WiFi) and an `apply` section (`passphrase`, `address`, `gateway`, `dns`,
`autoconnect`, `trusted`). The template documents every key. A profile with an
error is rejected on its own; one with a passphrase must be mode `0600`.

`interfaces` holds one block per interface:

```
enp8s0 {
	metric = 100
	table = casa.cable
	carrier-grace = 3000
}
wlp3s0 {
	wifi = no
}
```

`wifi = no` turns the WiFi of that interface off: no `wpa_supplicant`, the
interface is kept down and `STATUS` shows it `off`. On a wired interface the key
is an error of its block.

DNS goes to `/run/tundra/network/resolv.conf` and nowhere else: link
`/etc/resolv.conf` to it to follow the active connections, ordered by metric,
three servers at most.

## Behaviour

### Connections

- **Link**: a cable that is down is brought up with `tundra-ip link up` right
  before its connection starts; one with nothing to connect stays down. On WiFi,
  `wpa_supplicant` brings it up.
- **DHCP**: one `tundra-dhcpc` per interface, with the `metric`, `table` and
  `carrier-grace` of `interfaces` and `lease-key <profile>`. Its output goes to
  the log as `dhcpc[<if>/<profile>]: …`.
- **Fixed address**: probed with `tundra-blueice-arp probe`, then applied with
  `tundra-ip` (address, default route and, with `table`, route and rule). If the
  address is in use, it is not applied and a `static-taken` warning names the MAC
  that holds it; only a `CONNECT` tries again.
- **Sensor**: every connection with an address runs `tundra-blueice-arp watch`
  on it, and again when the address or the gateway change.
- **WiFi**: every wireless interface (one with `phy80211`) runs its own
  `wpa_supplicant -i <if> -D nl80211`, with a minimal configuration in
  `/run/tundra/network/wpa/`, driven through its control socket. Its output
  goes to the log as `wpa[<if>]: …`. If it exits unasked, it is relaunched with
  backoff, 1 s to 60 s.
- **WiFi networks**: every valid profile with `ssid` for that interface (or
  `interface = *`) is a network, with the profile name as `id_str`; the first
  profile has the highest priority and `autoconnect = no` leaves it disabled.
  `wpa3` is SAE with PMF required and OCV where `wpa_supplicant` has it,
  `wpa2` is WPA-PSK with PMF when the access point offers it, both CCMP only.
  `CONNECT` to a WiFi profile leaves only that network enabled; `DISCONNECT`
  disconnects `wpa_supplicant` until the next `CONNECT`. A reload replaces only
  the networks whose profile changed.
- **DHCP on WiFi**: `tundra-dhcpc` starts on association, with the profile of
  that network, or with the first profile without `ssid` that fits the
  interface for a network without profile (one added by hand with `wpa_cli`).
  `carrier-grace` is 3000 ms unless `interfaces` says otherwise. Another SSID
  releases it with `SIGUSR1` and starts another one; roaming within the same
  SSID touches nothing. A profile without `ssid` never starts on WiFi by
  itself.

| tundra-dhcpc exits with | tundra-networkd |
|-------------------------|-----------------|
| 1, or killed | Relaunches it with backoff, 1 s to 60 s |
| 1, interface removed | Waits for the interface to come back |
| 2 | Leaves the profile blocked until a reload |
| 111 | Follows the alert |
| 0, unasked | Leaves it stopped |

A child that does not stop within 5 s of being asked is killed.

### Alerts

Alerts come from the children (ADR-025 format) and from tundra-networkd itself
(`sensor-down`, `static-taken`). An alert signed by a package that may not use
that child's pipe is discarded.

- A `stop` releases the connection and leaves the interface stopped: only that
  one up to level 8, every interface from level 9.
- On WiFi, a `stop` up to level 8 disables that network in `wpa_supplicant`
  instead, and another network of the interface may connect; `force` goes back
  to it. From level 9, or on a network without a profile, the interface stops
  as a cable does, with `wpa_supplicant` disconnected. The pipe of
  `wpa_supplicant` carries no alerts.
- `ALERT-REPLY <id> force` reconnects, accepting that case up to that level for
  the whole connection; `disconnect` leaves the interface as after a
  `DISCONNECT`; `ack` closes a warning.
- An alert of level 6 or more makes the connection untrusted until it ends.
- Pending alerts are kept in `/run/tundra/network/alerts`: a restarted daemon
  keeps stopped what they stopped.

### Reload

| Change in the profile of an active connection | Effect |
|-----------------------------------------------|--------|
| None | Nothing |
| Only `dns` | `resolv.conf` is rewritten |
| Anything else, also in `interfaces` | Reconnects |
| Profile deleted | Disconnects |
| Profile with errors | Keeps the previous configuration until the next reload |

## Privileges

The unit runs the daemon as `tundra-network`, in the groups `tundra-network` and
`network`, with `CAP_NET_ADMIN`, `CAP_NET_RAW` and `CAP_NET_BIND_SERVICE` and a
read-only system (ADR-031). On start the daemon keeps those capabilities only as
inheritable: it cannot use them, and a child activates one only by running a
binary that carries it as a file capability (`+ei`), as `tundra-ip`,
`tundra-dhcpc`, `tundra-blueice-arp` and `wpa_supplicant` do. As root it changes
nothing.

Every child gets `TUNDRA_PARENT_PID`, the pid of the daemon: the kernel drops the
parent-death signal when a file capability takes effect, and the child arms it
again from that. `/usr/lib/sysusers.d/tundra-networkd.conf` creates the user and
the `network` group. Profiles with a passphrase belong at `0640
root:tundra-network`.

## Control socket

`/run/tundra/network.socket`, `0660 root:network` (`0600 root:root` while that
group does not exist). One command per line; each one answers `OK` or
`ERROR <reason>`, and lists end with `OK`.

| Command | Answer |
|---------|--------|
| `STATUS` | One `iface` line per interface |
| `PROFILES` | One `profile` line per profile, in load order |
| `CONNECT <profile> [interface]` | The interface is required for `interface = *` |
| `DISCONNECT <interface>` | Releases the connection |
| `RELOAD` | Rereads the configuration, as does `SIGHUP` |
| `SUBSCRIBE` | From then on, `event` and `ALERT` lines |
| `ALERTS` | One `ALERT` line per pending alert |
| `ALERT-REPLY <id> <option>` | One of the options the alert offers in `reply` |

```
iface enp8s0 state connected profile casa address 10.79.0.100/24 trusted no override - alert - ssid - bssid -
iface wlp3s0 state connected profile cafe address 10.80.0.119/24 trusted no override - alert - ssid caf%C3%A9 bssid 52:54:00:12:34:56
profile casa interface enp8s0 autoconnect yes trusted no ssid - security -
profile cafe interface wlp3s0 autoconnect no trusted no ssid caf%C3%A9 security wpa3
event connected enp8s0 casa address 10.79.0.100/24
ALERT 7 casa alert 111/8 stop by tundra-blueice-arp on enp8s0 ts 1762171512.347 reply force,disconnect info case D ...
```

`ssid` and `bssid` close every `iface` line, `ssid` and `security` every
`profile` line, with `-` on a cable; the ssid uses the `%XX` encoding of the
alerts (ADR-025) and the passphrase never leaves the daemon.

States: `idle`, `connecting`, `connected`, `stopping`, `retrying`, `waiting`,
`unmanaged`, `off` and `blocked`, with `reason config`, `exited`, `alert` or
`taken`.

## Exit status

| Code | Meaning |
|------|---------|
| `0` | Stopped by SIGTERM or SIGINT, after its children |
| `1` | Operational failure: unreadable configuration directory, a socket that cannot be opened, or another tundra-networkd answering on the control socket |
| `2` | Invalid usage |

## Files

| Path | Purpose |
|------|---------|
| `/run/tundra/network/resolv.conf` | DNS of the active connections |
| `/run/tundra/network/alerts`, `alert-id` | Pending alerts and the next id |
| `/run/tundra/network.socket` | Control socket |
| `/run/tundra/dhcpc/<if>-<profile>.lease` | Leases of tundra-dhcpc, watched with inotify |

## Building

```
make
make install PREFIX=/usr DESTDIR=<staging>
```

`TUNDRA_IP_PATH`, `TUNDRA_DHCPC_PATH` and `TUNDRA_BLUEICE_ARP_PATH` set where
the other tools are looked up
(`make TUNDRA_DHCPC_PATH=/usr/local/bin/tundra-dhcpc`). `make install` puts the
unit in `$(PREFIX)/lib/systemd/system/` and never overwrites a profile already in
`/etc/tundra/network/`. The Makefile honours the packager's `CPPFLAGS`, `CFLAGS`
and `LDFLAGS`.

## Tests

```
make check-unit
make check
```

- `check-unit` runs the unit tests (`tests/unit/t-*.c`) built with
  AddressSanitizer and UBSan. No root needed.
- `check` runs the unit tests and then the network scenarios
  (`tests/net/t-*.sh`) in network namespaces. They need root (the runner
  re-executes itself with `sudo`), `iproute2` and `dnsmasq`.
- The DHCP scenarios run the test build of `tundra-dhcpc` found at
  `../tundra-dhcpc/build/test/tundra-dhcpc` (its own `make check` leaves it
  there), or the binary in `DHCPC_REAL`. The attack scenarios also run the test
  build of `tundra-blueice-arp` and its `arpsend` tool from
  `../tundra-blueice-arp/build/test/` (or `BIARP_REAL`). The others use fakes
  built from `tests/tools/`. `tundra-ip` is taken from `/usr/local/bin` (or
  `TIP_REAL`).
- The WiFi scenarios create three `mac80211_hwsim` radios inside the test
  namespaces, out of reach of any wireless manager on the host, and leave the
  module loaded without radios. They need `hostapd` (or `HOSTAPD_REAL`), `iw`,
  `wpa_cli` and `wpa_supplicant` (`/usr/bin/wpa_supplicant`, or `WPA_REAL`).
- The OCV scenario needs a `hostapd` built with `CONFIG_OCV` and
  `CONFIG_TESTING_OPTIONS` in `HOSTAPD_REAL`, and is skipped without it.

## Currently unsupported

- WiFi Enterprise (EAP) profiles.
- IPv6.
- Sandboxing directives in the unit and dropping privileges.
- An event when another client answers an alert.
