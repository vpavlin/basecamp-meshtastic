# Meshtastic Gateway — message data flows & anti-spam design

This document pins down exactly how messages move between the **Meshtastic mesh** (a LoRa
channel) and a **Logos Messaging topic**, and the rules that guarantee we never spam either
network (no relay loops, no echo storms, no duty-cycle abuse).

## Entities

```
        ┌───────────────┐         ┌─────────────────┐         ┌───────────────────┐
        │  Meshtastic   │  LoRa   │     Gateway     │  Waku   │  Logos Messaging  │
        │  mesh (ch N)  │◀───────▶│   (this module) │◀───────▶│  topic T(N)       │
        └───────────────┘         └─────────────────┘         └───────────────────┘
              radio                    bridge + dedup               internet
```

- **Channel N** — a Meshtastic channel slot (0–7), each with name + PSK + role.
- **Topic T(N)** — the Logos Messaging content topic, derived `md5(name+psk)[:16]`
  (or `md5("idx:N")` when the channel name is empty). Membership-gated: only someone holding
  the channel name+PSK can compute it. See `deriveTopic()`.
- **Gateway** — bridges N ⇄ T(N), but **only for channels the user has toggled `relaying`**.

## Message origin (the key tag)

Every message the gateway handles is tagged with where it entered from:

| origin   | meaning                                                              |
|----------|---------------------------------------------------------------------|
| `mesh`   | heard on the LoRa channel (someone else's node, or our own TX echo) |
| `lm`     | received on the Logos Messaging topic via delivery/Waku             |
| `local`  | typed by the user in this gateway UI                                |

## The core invariant

> **Each message crosses the bridge at most once, and is never sent back onto the network it
> came from.**

We enforce this with a **dedup fingerprint set** `seen`:

- `fingerprint = md5(channelIndex + "\n" + normalize(text))` (today: text-based; see "Robust
  identity" below for the production version using source-assigned packet ids).
- `seen` is **bounded** (FIFO, cap ~256) and **short-lived** (entries expire, ~60 s) so that a
  *legitimately repeated* message later on is not suppressed forever.
- Rule, applied before any relay: **if `fingerprint ∈ seen` → SUPPRESS** (it is an echo of
  something we already bridged). **else → insert `fingerprint`, then relay.**

Critically, we insert the fingerprint **before** emitting to the other network, so the echo
that inevitably comes back is already in `seen`.

## Flows

### Flow A — message originates on the mesh

```
mesh(ch N) ──▶ Gateway [origin=mesh]
                  │ 1. show in chat (incoming)
                  │ 2. relaying? fp∉seen?  ── no ─▶ done (display only)
                  │         yes
                  │ 3. seen += fp ; publish → T(N)        [relayed ✓]
                  ▼
              LM topic T(N)
                  │ 4. ...echoes back to our own subscription
                  ▼
            Gateway [origin=lm, same text]
                  │ 5. fp ∈ seen  ─▶ SUPPRESS (do NOT re-inject to mesh)   ← loop broken
```

### Flow B — message originates on Logos Messaging (e.g. the laptop UI publishes)

```
LM topic T(N) ──▶ Gateway [origin=lm]
                  │ 1. show in chat (incoming, "from Logos Messaging")
                  │ 2. relaying? fp∉seen?  ── no ─▶ done (display only)
                  │         yes
                  │ 3. seen += fp ; mesh.send(ch N, text)  [relayed ✓]
                  ▼
              mesh(ch N)
                  │ 4. our own TX — Meshtastic does not normally loop our packets back,
                  │    but IF it did [origin=mesh, same text]:
                  │ 5. fp ∈ seen  ─▶ SUPPRESS                              ← loop broken
```

### Flow C — local send (user types in this UI)

```
user ──▶ Gateway [origin=local]
            │ 1. seen += fp                       (BEFORE sending, so both echoes are covered)
            │ 2. mesh.send(ch N, text)            (always — it's a chat message)
            │ 3. relaying?  yes ─▶ publish → T(N) [relayed ✓]
            ▼
   echoes: LM→[origin=lm] and/or mesh→[origin=mesh], same text
            │ 4. fp ∈ seen ─▶ SUPPRESS both                               ← no self-loop
```

## Implementation — how each leg actually connects (code map)

Both legs are implemented and verified against a physical Meshtastic node (bidirectional).
Everything lives in `gateway/src/meshtastic_gateway_plugin.cpp`.

### Mesh leg — native USB serial (QtSerialPort + meshtastic protobufs)

No Python, no subprocess: the module speaks the Meshtastic **StreamAPI** directly. The radio
decrypts on-air traffic, so over the wire we only ever see **plaintext** (no crypto in the gateway).

```
openSerial()            autodetect USB (Espressif/SiLabs/CH34x vid, or $MESHTASTIC_DEV), open @115200,
                        send ToRadio{want_config_id}
onSerialReadyRead()     reassemble StreamAPI frames: 0x94 0xC3 <lenHi> <lenLo> <protobuf>
handleFromRadio()       dispatch one FromRadio:
                          • MyNodeInfo   → our node num
                          • NodeInfo×N   → node DB: total + online (last_heard < 2h) + name map
                          • Channel×N    → {index, name, PSK, role}  (real name+PSK)
                          • config_complete_id → finalizeConfig()
                          • MeshPacket(TEXT_MESSAGE_APP) → onMeshMessage(ch, sender, text)
finalizeConfig()        rebuildChannelsFromMesh(): real name+PSK → deriveTopic() → real T(N);
                        push node stats; mark node present
sendToMesh(ch, text)    frame ToRadio{MeshPacket{to=BROADCAST, channel=ch, Data{TEXT, payload}}}
```

### Logos Messaging leg — depend on `delivery_module` via Logos Core IPC

We do **not** bundle a Waku node — the gateway reuses the one `delivery_module` runs, over IPC
(`getClient`/`requestObject`/`onEvent`/`invokeRemoteMethodAsync`).

```
initDelivery()          createNode({mode:Edge, preset:logos.dev}) → start → subscribeRelayTopics()
subscribeRelayTopics()  delivery_module.subscribe(T(N)) for each relaying channel
publishToLM(ch, text)   AES-256-GCM encrypt (key = SHA256(domain+psk)) → "ENC1:"+b64(iv|ct|tag) →
                        delivery_module.send(T, payload)   (no-PSK channels are sent plaintext)
onDeliveryMessage()     messageReceived [hash, topic, payloadB64, ts] → decode → decrypt ENC1 (drop on
                        auth failure) → dedup (on plaintext) → display
                        + sendToMesh()   (inject onto LoRa)
```

### Flow → function map

| Flow step                                  | Code                                                   |
|--------------------------------------------|--------------------------------------------------------|
| mesh inbound, dedup, display, relay→LM     | `onMeshMessage()` → `maybeRelay(ch,text,"mesh")` → `publishToLM()` |
| LM inbound, dedup, display, inject→mesh     | `onDeliveryMessage()` → `sendToMesh()`                 |
| local send (UI)                            | `sendMessage()` → `maybeRelay(ch,text,"local")` (LM) + `sendToMesh()` (mesh) |
| the one suppression rule                   | `maybeRelay()` / the `m_seen` check in `onDeliveryMessage()`+`onMeshMessage()` |
| relay toggle → (un)subscribe T(N)          | `setRelay()`                                           |

## Anti-spam guarantees (why neither network gets flooded)

1. **Loop prevention** — the `seen` fingerprint set above. This is the one that prevents the
   runaway mesh↔LM echo storm. Without it a single message ping-pongs forever.
2. **Opt-in relay** — a channel is bridged only when the user explicitly toggles `relaying`.
   Default is **off**. The public/default channel (index 0) is never auto-relayed.
3. **LoRa airtime discipline** — LoRa is duty-cycle limited and slow. The mesh→ and →mesh paths
   must:
   - truncate to the Meshtastic payload limit (~228 bytes; longer = drop or chunk, never silently
     spam),
   - rate-limit / queue with backpressure (a burst on LM must not become a burst on LoRa),
   - drop (with a logged warning) rather than buffer unboundedly when the mesh is saturated.
4. **No catch-up amplification** — Logos Messaging has no Store replay in our edge config, and we
   do **not** replay history onto the mesh on (re)connect. Only live messages bridge.
5. **Bounded, expiring dedup state** — `seen` is capped and entries expire, so memory is bounded
   and legitimate repeats eventually pass.

## Robust identity (current vs. future)

Text-based fingerprints have one weakness: two *distinct* messages with identical text inside the
dedup window collapse to one (one gets suppressed). Acceptable for a PoC, rare in practice.

The production fingerprint should key on **source-assigned ids** instead of text:
- Meshtastic packets carry a **packet id** (`from` + `id`); use it for the mesh side.
- Waku/LM messages have a **message hash**; use it for the LM side.
- Maintain a small cross-map so a mesh packet id and the LM hash of *the same bridged message*
  are recognized as the same logical message. Then identical text is never wrongly suppressed,
  and echoes are matched exactly.

## Failure modes (documented, bounded)

| Situation                            | Behavior                                                        |
|--------------------------------------|----------------------------------------------------------------|
| Gateway restart                      | `seen` is empty; a single in-flight message may double-bridge once, then settles. |
| Relay toggled off mid-flight         | New messages stop bridging immediately; in-flight echoes still suppressed by `seen`. |
| LM offline                           | mesh→LM messages dropped (logged); no queue/replay. Mesh chat unaffected. |
| Mesh offline / saturated             | LM→mesh messages dropped (logged) rather than buffered unboundedly. |
| Same text sent twice within window   | Second bridge suppressed (text-fp limitation; fixed by source-id identity above). |

## UI surfacing

- **Per-message relay tick** — a message shows `⇄ relayed to Logos Messaging` (it crossed the
  bridge) or `⇄ from Logos Messaging` (origin=lm). Distinct from the mesh **delivery ack**
  (`✓✓ delivered`).
- **Suppressed echoes are invisible** by design — they're logged (`dedup — suppressing <origin>
  echo`) but never shown or re-sent.
- **Node stats** — the sidebar shows how many mesh nodes the local node currently sees and how
  many are "online" (heard within the activity window). Sourced from the Meshtastic NodeDB.
