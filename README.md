# tundra-networkd

Network orchestrator, developed for Tundra OS. It connects the networks described
in the `.tnc` profiles of `/etc/tundra/network/`, supervises `tundra-dhcpc`,
`wpa_supplicant` and the `tundra-blueice` sensors, writes `resolv.conf` and
publishes state and alerts on a control socket. It never decides on its own: it
runs what the profiles and the user ask for.

DNS goes to `/run/tundra/network/resolv.conf` and nowhere else: link
`/etc/resolv.conf` to it to follow the active connections.

## Control socket

`/run/tundra/network.socket`, `0660 root:network` (`0600 root:root` while that
group does not exist). One command per line; each one answers `OK` or
`ERROR <reason>`, and lists end with `OK`.

| Command | Answer |
|---------|--------|
| `STATUS` | One `iface` line per interface |
| `PROFILES` | One `profile` line per profile, in load order |
| `CONNECT <profile> [interface]` | The interface is required for `interface = *` |
| `DISCONNECT <interface>` | Releases the lease |
| `RELOAD` | Rereads the configuration, as does `SIGHUP` |
| `SUBSCRIBE` | From then on, `event connected` and `event disconnected` lines |

```
iface enp8s0 state connected profile casa address 10.79.0.100/24 trusted no
profile casa interface enp8s0 autoconnect yes trusted no
event connected enp8s0 casa address 10.79.0.100/24
```

## Requirements

- Linux.
- Root.
- `tundra-ip` 1.7_1, `tundra-dhcpc` 2.0_0 and `tundra-blueice-arp` 0.1_0 or later,
  at the paths fixed at build time (`TUNDRA_IP_PATH`, `TUNDRA_DHCPC_PATH`,
  `TUNDRA_BLUEICE_ARP_PATH`, under `/usr/bin` by default).

## Building

```
make
make install PREFIX=/usr DESTDIR=<staging>
```

The Makefile honours the packager's `CPPFLAGS`, `CFLAGS` and `LDFLAGS`.

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
  there), or the binary in `DHCPC_REAL`. The others use a fake dhcpc built from
  `tests/tools/dhcpc-fake.c`.
