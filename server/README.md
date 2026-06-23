# Headless / server relay

Run the gateway as an always-on relay on a server or Raspberry Pi — **no GUI, no laptop**.
The relay logic lives entirely in the **core** module; the Basecamp UI is just a remote control.
Headless, the core opens the radio, connects to Logos Messaging, and bridges the channels you've
marked for relay — on its own.

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

## Raspberry Pi / aarch64

Same approach, but the whole stack must exist for `aarch64`:

1. **Module binaries** — the release CI builds `linux-arm64` `.lgx` for the gateway + UI alongside
   `linux-amd64` (see [`.github/workflows/release.yml`](../.github/workflows/release.yml)); grab the
   arm64 assets. `delivery_module` / `capability_module` / `logoscore` come from the **Logos platform**
   and must also be aarch64 builds — that part is outside this repo.
2. **Known host bug (older ~early-2026 aarch64 `logoscore`):** modules that *declare dependencies*
   (this one declares `delivery_module`) can crash `logos_host` **before** `initLogos` runs — load
   order can't fix it. If your Pi build hits this: use a newer `logoscore`, or fall back to driving
   `liblogosdelivery` directly from a small Python/ctypes relay (the route the stoa relay took).
   On a current x86_64 host this does **not** happen (validated).
3. **Self-exit:** some builds let the core exit (~90 s) even when resident — `Restart=always` in the
   unit covers it.

On an **x86_64 server it works today**; on a Pi, verify against points 2–3 first.

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
