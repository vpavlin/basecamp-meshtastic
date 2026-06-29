# Bridge your LoRa mesh to Logos Messaging

The **Mesh Gateway** is a [Logos Basecamp](https://github.com/logos-co/logos-basecamp)
module that bridges a USB‑connected LoRa radio (**MeshCore** or **Meshtastic**) to
**Logos Messaging** (Waku). Messages on your mesh channels are relayed to and from
the Logos network, so your off‑grid LoRa mesh and the internet‑side world can talk.

This guide takes you from zero to a running, DWeb‑Camp‑configured gateway in about
5 minutes.

> **Platform:** Linux **x86_64** for now. macOS and Raspberry Pi (arm64) support is
> on the way — the gateway modules are currently published for amd64 only.

---

## What you need

- A LoRa radio running **MeshCore companion‑radio (USB)** firmware — or a
  **Meshtastic** node — connected to your computer over **USB**.
  *(Need to flash MeshCore? See the flashing guide on mesh.dod.ngo.)*
- A Linux **x86_64** machine.
- ~5 minutes.

You can do **steps 1–4 ahead of time** (download + install) and **steps 5–7 at the
event** (plug in, configure, go).

---

## 1. Download Basecamp

Get the Logos Basecamp **0.2.0‑RC3** AppImage from the release page:

➡️ https://github.com/logos-co/logos-basecamp/releases/tag/0.2.0-RC3

Download `LogosBasecamp-Desktop-…-x86_64.AppImage`, then make it executable:

```bash
chmod +x LogosBasecamp-Desktop-*-x86_64.AppImage
```

> AppImages need FUSE. It's preinstalled on most distros; if not:
> `sudo apt install libfuse2`

---

## 2. Plug in your radio and launch Basecamp

Connect your LoRa radio over USB, then launch Basecamp **in MeshCore mode**:

```bash
MESH_PROTOCOL=meshcore ./LogosBasecamp-Desktop-*-x86_64.AppImage
```

- Using a **Meshtastic** node instead? Just launch without the variable:
  `./LogosBasecamp-Desktop-*-x86_64.AppImage`
- The gateway auto‑detects your USB radio. If you have several serial devices,
  point it at the right one:
  ```bash
  MESH_PROTOCOL=meshcore MESHTASTIC_DEV=/dev/ttyACM0 ./LogosBasecamp-Desktop-*-x86_64.AppImage
  ```

> **Serial permission (Linux):** if the radio isn't detected, add yourself to the
> `dialout` group once, then log out and back in:
> `sudo usermod -aG dialout $USER`

---

## 3. Add the module repository

In Basecamp, open the **Package Manager** and go to **Package Repositories**, then
**Add repository** and paste:

```
https://raw.githubusercontent.com/vpavlin/basecamp-meshtastic/master/repo/logos-repo.json
```

Refresh — you'll see **Basecamp Mesh Gateway** offering three packages.

---

## 4. Install the modules

Install all three:

| Package | What it is |
|---|---|
| **`mesh_gateway`** | the gateway backend (talks to your radio) |
| **`mesh_gateway_ui`** | the user interface |
| **`qr`** | renders QR codes for sharing channel links |

> A **"Package is unsigned"** warning is expected for community modules — install anyway.

If the gateway doesn't appear in your apps right after installing, **restart Basecamp**
(relaunch with the same command from step 2) so the new modules register.

---

## 5. Open the gateway and connect

Open **Mesh** from the sidebar. It connects to your USB radio and shows:

- a **MeshCore** (or **Meshtastic**) badge,
- your radio's **channels**,
- a **Nodes** view of other radios it has heard.

In the channel list you choose **which channels bridge to Logos Messaging**. Bridged
channels relay both ways.

> **Privacy:** only bridge channels you intend to be shared on Logos. Don't bridge
> your radio's default/Public channel.

---

## 6. Configure for DWeb Camp

1. In the gateway, open **Settings**.
2. Under **Load mesh config**, click **DWeb Camp**, then **Load**.

This tunes your radio (frequency, spreading factor, …) and adds the DWeb Camp
channels (`#dwebcamp`, `#schedule`, `#workshop`, …) in one shot.

> Every radio that should mesh together must load the **same** config — the DWeb Camp
> preset guarantees that.

---

## 7. Verify it works

- **Nodes:** within a couple of minutes, other DWeb Camp radios appear — they show
  **online** as soon as they send anything.
- **Messages:** post on **#dwebcamp** from another radio; it appears in the gateway
  and, for bridged channels, relays onto Logos Messaging.
- **Share a channel:** open a channel's share view to get a **QR code** others can scan.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Radio not detected | Use a **data** USB cable (not charge‑only); add yourself to `dialout`; connect just one radio, or set `MESHTASTIC_DEV=/dev/ttyACMx`. |
| Modules don't show after install | Restart Basecamp (relaunch from step 2). |
| Gateway loads but no channels | Make sure you launched with `MESH_PROTOCOL=meshcore` for a MeshCore radio. |
| "QR unavailable" | Install the **`qr`** package (step 4). |

---

## Links

- **Gateway repo / source:** https://github.com/vpavlin/basecamp-meshtastic
- **Logos Basecamp:** https://github.com/logos-co/logos-basecamp
