# basecamp-meshtastic

Logos Basecamp modules that bridge **Logos Messaging (Delivery / Waku)** to a **Meshtastic LoRa**
mesh — chat from a Basecamp UI and have it reach (and come back from) the mesh.

## Modules

- **`ui/`** — `meshtastic_gateway_ui` (`ui_qml`): the chat UI. Publishes/reads a Logos Messaging
  content topic via `delivery_module`. Built with the Logos design system. Works standalone (you
  can chat over Delivery with no mesh attached).
- **`gateway/`** — `meshtastic_gateway` (`core`): discovers a connected Meshtastic node and bridges
  the same content topic to/from the mesh. The Logos Messaging side is wired; the Meshtastic
  transport (serial/BLE + protobufs) is a TODO behind `sendToMesh()`/`onMeshFrame()`.

Both depend on `delivery_module` and coordinate via a shared **content topic**
(default `/meshtastic-gateway/1/chat/proto`). The UI does not depend on the gateway — the gateway is
an optional bridge that runs wherever a Meshtastic node is attached.

## Build & install (into Basecamp Dev)

```bash
cd ui && nix build .#lgx        # -> result/*.lgx
# install via lgpm into ~/.local/share/Logos/LogosBasecampDev/{modules,plugins}
# NOTE: for a ui_qml the installed manifest's "main" must map the variant -> Main.qml
#       e.g. "main": {"linux-amd64-dev": "Main.qml"}  (an empty {} makes Basecamp skip it)
```

See the `basecamp-deploy` / `package-basecamp-module` skills for the iterate loop.

## Notes

- The headless van Pi can't run a Basecamp core module reliably (host limitation); there the
  Delivery⇄LoRa bridge runs as a small Python service driving `liblogosdelivery` via ctypes
  (`stoa/rpi/stoa/relay.py`). `meshtastic_gateway` targets machines running Basecamp's GUI with a
  Meshtastic node on USB.
- `delivery_module` here is the local working tree with `events` declared and the broken
  `interface:"universal"` codegen removed (so it builds + exposes `.on()` to consumers).
