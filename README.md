# tundra-networkd

Network orchestrator, developed for Tundra OS. It connects the networks described
in the `.tnc` profiles of `/etc/tundra/network/`, supervises `tundra-dhcpc` and
the `tundra-blueice` sensors, applies fixed addresses, writes `resolv.conf` and
publishes state and alerts on a control socket. It never decides on its own: it
runs what the profiles and the user ask for.

## Requirements

- Linux.
- Root.
- `tundra-ip` 1.7_1, `tundra-dhcpc` 2.0_0 and `tundra-blueice-arp` 0.1_0 or later,
  at the paths fixed at build time (`TUNDRA_IP_PATH`, `TUNDRA_DHCPC_PATH`,
  `TUNDRA_BLUEICE_ARP_PATH`, under `/usr/bin` by default).
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
```

DNS goes to `/run/tundra/network/resolv.conf` and nowhere else: link
`/etc/resolv.conf` to it to follow the active connections, ordered by metric,
three servers at most.

## Behaviour

### Connections

- **DHCP**: one `tundra-dhcpc` per interface, with the `metric`, `table` and
  `carrier-grace` of `interfaces` and `lease-key <profile>`. Its output goes to
  the log as `dhcpc[<if>/<profile>]: …`.
- **Fixed address**: probed with `tundra-blueice-arp probe`, then applied with
  `tundra-ip` (address, default route and, with `table`, route and rule). If the
  address is in use, it is not applied and a `static-taken` warning names the MAC
  that holds it; only a `CONNECT` tries again.
- **Sensor**: every connection with an address runs `tundra-blueice-arp watch`
  on it, and again when the address or the gateway change.

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
iface enp8s0 state connected profile casa address 10.79.0.100/24 trusted no override - alert -
profile casa interface enp8s0 autoconnect yes trusted no
event connected enp8s0 casa address 10.79.0.100/24
ALERT 7 casa alert 111/8 stop by tundra-blueice-arp on enp8s0 ts 1762171512.347 reply force,disconnect info case D ...
```

States: `idle`, `connecting`, `connected`, `stopping`, `retrying`, `waiting`,
`unmanaged` and `blocked`, with `reason config`, `exited`, `alert` or `taken`.

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

## Currently unsupported

- WiFi: profiles with `ssid` are loaded and validated, but never match.
- IPv6.
- Sandboxing directives in the unit and dropping privileges.
- An event when another client answers an alert.
