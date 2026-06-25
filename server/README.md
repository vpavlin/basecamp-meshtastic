# Headless / server relay

Run the gateway as an always-on relay on a server or Raspberry Pi — **no GUI, no laptop**.
The relay logic lives entirely in the **core** module; the Basecamp UI is just a remote control.
Headless, the core opens the radio, connects to Logos Messaging, and bridges the channels you've
marked for relay — on its own.

> ## ⚠️ Direction support: headless is currently **mesh → Logos Messaging only**
>
> Under bare `logoscore`, the relay is **one-way**: messages from the mesh are bridged **to** Logos
> Messaging, but the reverse (**Logos Messaging → mesh**) does **not** work headlessly. This is
> upstream **[logos-basecamp#150](https://github.com/logos-co/logos-basecamp/issues/150)**: the
> capability-token bootstrap that authorizes cross-module *events* (`delivery_module` →
> our gateway's `messageReceived`) is performed by the **Basecamp GUI host**, not by `logoscore`.
>
> What we verified (2026-06-25): an inbound Logos Messaging message **does reach the Pi's
> `delivery_module`** (it logs `Received message push` and emits `messageReceived`), but the gateway's
> `onDeliveryMessage` never fires under `logoscore` — `informModuleToken` returns false with
> `QtProviderObject::informModuleToken: LogosAPI not available` (capability isn't bootstrapped). A
> gateway-side `TokenManager` seed (see `initDelivery()`) does **not** fix it — the gap is capability-side.
>
> **Workarounds for full bidirectional:**
> - Run the full **Basecamp GUI** under a virtual display (`xvfb`) — it performs the bootstrap, so
>   events flow. Heavier, and on aarch64/xvfb it has rough edges (flaky Waku peering, suppressed module
>   logs, the gateway can be slow to subscribe). This is how the reference deployment runs it.
> - The durable fix is upstream #150 — once landed, this lightweight `logoscore` daemon does both
>   directions with full visibility.

## How it runs

There's no separate daemon binary — the GUI-less Logos host is **`logoscore`** (the CLI sibling of
`logos-basecamp`, same plugin host, no Qt/QML). "Daemon mode" is just running it resident, loading
the gateway and its dependencies:

```bash
logoscore -m <modules-dir> -l capability_module,delivery_module,meshtastic_gateway
```

- `capability_module` — token handshake, `delivery_module` — Waku, `meshtastic_gateway` — the bridge.
  Order matters (deps first). `qr` is UI-only and **not** needed headless.
- **Which channels are relayed comes from the gateway's SQLite**
  (`~/.local/share/logos_host/meshtastic_gateway/gateway.db`). Set it once with `gwctl` (below), and
  every restart resumes those channels. Channel 0 (public/default) is never relayed.

## Install (systemd --user)

Prereqs: a `logoscore` for your arch, the gateway + `delivery_module` + `capability_module` installed
under a modules dir (install the `.lgx` as in the main README), a Meshtastic node on USB, and your
user in the `dialout` group.

```bash
git clone https://github.com/vpavlin/basecamp-meshtastic && cd basecamp-meshtastic/server
# auto-detects logoscore + the modules dir; override with LOGOSCORE=... MODULES_DIR=...
./install.sh
```

This drops `meshtastic-gateway-run` + `gwctl` into `~/.local/bin`, writes
`~/.config/meshtastic-gateway.env` (the resolved paths), installs the user unit, enables it, and turns
on linger (so it runs without you being logged in).

## Configure without the UI — `gwctl`

`gwctl` is the headless control surface. Each command is a short `logoscore` run that loads the module,
waits for the radio's config burst, calls the matching `Q_INVOKABLE`, and persists to SQLite.

> The daemon and `gwctl` share the serial port + DB, so **stop the service while configuring**:

```bash
systemctl --user stop meshtastic-gateway

gwctl channels                 # list channels: index, relay state, psk type, name
gwctl relay 1 on               # bridge channel 1  (refuses channel 0)
gwctl relay 1 off              # stop bridging it
gwctl join 'https://meshtastic.org/e/#Ch8…'   # join an existing channel by share link
gwctl name "Relay Node" RLY    # set the node's display name (written to the radio)
gwctl set onlineWindowSec 7200 # settings: onlineWindowSec | maxMsgsPerChannel | distanceUnit
gwctl status                   # node + delivery status

systemctl --user start meshtastic-gateway
journalctl --user -u meshtastic-gateway -f      # watch it relay
```

Channels themselves come from the **radio** — configure those with the standard Meshtastic app/CLI (or
`gwctl join`); `gwctl relay` only decides which of them this gateway bridges.

## Raspberry Pi / aarch64 — validated recipe (2026-06-23, Pi 5 / Debian 13)

This runs end-to-end on a Pi: gateway connected to the radio, Waku ready, `LM Relayed1` relaying.
The aarch64 Logos runtime isn't cleanly packaged yet, so assembling it takes a few manual steps.
Components (all aarch64):

| Piece | Source |
|---|---|
| `logoscore` + `logos_host` + Qt 6.9.2 | extract `logoscore-aarch64.AppImage` from a [logos-liblogos release](https://github.com/logos-co/logos-liblogos/releases) (`./x.AppImage --appimage-extract`) |
| `capability_module` (aarch64) | from the [logos-basecamp aarch64 AppImage](https://github.com/logos-co/logos-basecamp/releases) (`usr/modules/capability_module`) |
| `delivery_module` (aarch64 `.lgx`) | `nix build github:logos-co/logos-delivery-module#lgx-portable` on an arm64 runner |
| gateway core + UI (aarch64 `.lgx`) | our Release `…-linux-arm64.lgx` |

Assemble a modules dir — for each `.lgx`: `tar xzf x.lgx`, copy `variants/linux-arm64/*` + `manifest.json`
into `modules/<name>/`, write `linux-arm64` into a `variant` file. Then the **gotchas that bit us**:

1. **Variant naming.** A *dev* `logoscore` looks for `linux-arm64-dev` (not `linux-arm64`) in each
   `manifest.json`'s `main`. Add the key: `main["linux-arm64-dev"] = main["linux-arm64"]`.
2. **Missing `libQt6SerialPort`.** The GUI bundle ships Qt 6.9.2 with `Sql`/`RemoteObjects`/`Network`
   but **not** `SerialPort`. Drop a matching Qt 6.9.x `libQt6SerialPort.so.6` into the bundle's
   `usr/lib` (`nix build nixpkgs#qt6.qtserialport --system aarch64-linux`).
3. **SQLite driver dep.** The bundled `libqsqlite.so` needs `libsqlite3.so`; symlink the system one:
   `ln -s /usr/lib/aarch64-linux-gnu/libsqlite3.so.0 <bundle>/usr/lib/libsqlite3.so` — else the DB
   (relay-pref persistence) silently fails.
4. **Cleanup process name.** The aarch64 host is `.logos_host.elf` (x86 is `.logos_host-wra`) — the
   wrapper/`gwctl` use `pkill -f logos_host` to match both. A leaked host holds the radio and the next
   start gets `linkState:noperm`.
5. Run with `QT_QPA_PLATFORM=offscreen` and `LC_ALL=C.UTF-8`.

Then add the channel + enable relay headlessly and start the unit:
```bash
gwctl join 'https://meshtastic.org/e/#…'   # adds the channel to the radio
gwctl relay 2 on                            # persists to SQLite
systemctl --user enable --now meshtastic-gateway
```

**The dep-crash bug is NOT present** in current builds — `delivery_module` (a dependency-declaring
module) and our gateway both load and `initLogos` runs (validated on the Pi). The `~90 s` self-exit
didn't occur either, but `Restart=always` covers it; relay prefs persist in SQLite so a restart resumes
relaying. On an **x86_64 server** none of steps 1–3 are needed — the dev Basecamp's Qt already matches.

## Files

| File | Purpose |
|---|---|
| `meshtastic-gateway-run` | the daemon command (cleans stale hosts, execs `logoscore`) |
| `gwctl` | headless configuration (relay toggles, join, name, settings) |
| `meshtastic-gateway.service` | systemd `--user` unit (`Restart=always`) |
| `install.sh` | installs the above + enables the service |

## Troubleshooting

- **`a logos host is already running`** from `gwctl` → stop the service first.
- **stuck "connecting" / can't open serial** → another process holds the radio, or the user isn't in
  `dialout`. Only one process can hold the node at a time.
- **`Address already in use` (Waku)** → a leaked host from a crash; the wrapper kills `.logos_host-wra`
  on start, or do it manually: `pkill -x .logos_host-wra`.
