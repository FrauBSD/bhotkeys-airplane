[//]: # ($FrauBSD: bhotkeys-airplane/README.md 2026-10-05 18:44:10 -0700 Devin Teske $)

# bhotkeys-airplane

Airplane-mode keys for FreeBSD X11. `Super+A` and `XF86RFKill`
run the same toggle. `airplane-radiosd` is the root service that
takes the WLAN interface down and quiets Bluetooth. The session
talks to its socket. bosd paints `airplane-on` or `airplane-off`.

On FreeBSD, Fn+F10 arrives as `KEY_RFKILL` through `hrfkill(4)`
and is mapped to `XF86RFKill`. Disable one of the two plugins if
a single press toggles twice.

Home: [FrauBSD/bhotkeys-airplane](https://github.com/FrauBSD/bhotkeys-airplane)

## Requirements

- `bhotkeys`
- `bosd`
- `hrfkill(4)` when the laptop emits the radio key that way

## Build / install

```sh
make
make install    # PREFIX=/usr/local by default
make clean
```

Enable the service:

```sh
sysrc airplane_radiosd_enable=YES
service airplane_radiosd start
```

Optional `rc.conf` settings: `airplane_radiosd_wlan` (default
`wlan0`), `airplane_radiosd_socket`, and `airplane_radiosd_group`
(default `airplane_radiosd`).

The socket is mode 0660 and that group. The service creates the
group. Add the session account, then log in again so the new
group is in the credentials:

```sh
pw groupmod airplane_radiosd -m user
```

Installs `airplane-mode` into
`${PREFIX}/bin`, `airplane-radiosd` into
`${PREFIX}/sbin`, its rc script into `${PREFIX}/etc/rc.d`, and
`fn-airplane` and `fn-rfkill` into
`${PREFIX}/share/bhotkeys/plugins.d`. Both plugins are offered
at the greeter.
