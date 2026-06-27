#ifndef MESH_RADIO_H
#define MESH_RADIO_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QJsonArray>

// Abstraction over a LoRa-mesh radio attached to this host over USB serial. Concrete backends speak a
// specific firmware's host protocol:
//   - MeshtasticRadio — Meshtastic StreamAPI (0x94 0xc3 framing + protobuf)
//   - MeshCoreRadio    — MeshCore Companion Radio protocol ('<'/'>' framing + binary command set)
//
// The gateway plugin owns the mesh-AGNOSTIC half (Logos Messaging relay, AES-GCM payload encryption,
// loop-prevention/dedup, SQLite history, the signal-driven UI) and drives a MeshRadio* purely through
// this interface. A backend autodetects/opens the port, runs the handshake, discovers the node's
// channels and NodeDB/contacts, reports inbound text, and transmits outbound text.
//
// Channel object schema emitted by channelsDiscovered() (one per usable channel):
//   { "channelIndex": int, "name": string, "psk": base64-string, "role": "PRIMARY"|"SECONDARY"|"" }
// "psk" is the per-channel secret key used to derive the LM topic + payload-encryption key
// (Meshtastic PSK / MeshCore 16-byte channel secret), so the plugin handles both uniformly via
// deriveTopic()/channelKey(). Empty "psk" => unencrypted channel (plaintext on LoRa and on LM).
class MeshRadio : public QObject
{
    Q_OBJECT
public:
    explicit MeshRadio(QObject* parent = nullptr) : QObject(parent) {}
    ~MeshRadio() override = default;

    virtual QString protocol() const = 0;   // "meshtastic" | "meshcore" (logging/UI)

    // Begin connecting: open/autodetect the serial port, run the firmware handshake, discover
    // channels + nodes. Idempotent; retries internally until connected. Emits linkStateChanged
    // throughout, then channelsDiscovered + nodesDiscovered once the config burst completes.
    virtual void start() = 0;
    virtual void stop() = 0;

    // Transmit text onto a mesh channel (channelIndex as reported by channelsDiscovered).
    virtual void sendToMesh(int channelIndex, const QString& text) = 0;

    // Radio-side config operations. Backends best-effort; may no-op what the firmware can't do.
    virtual void requestNodes() = 0;                                        // re-emit nodesDiscovered
    virtual void setOwner(const QString& longName, const QString& shortName) = 0;
    virtual void createChannel(const QString& name, const QByteArray& key) = 0;
    virtual void addChannelFromUrl(const QString& url) = 0;                  // firmware "share channel" link
    virtual void deleteChannel(int channelIndex) = 0;

signals:
    // The node's full current channel set (see schema above). Re-emitted whenever it changes.
    void channelsDiscovered(const QJsonArray& channels);

    // Inbound text heard on the mesh. channelIndex matches channelsDiscovered; from is a display name.
    void meshMessage(int channelIndex, const QString& from, const QString& text);

    // The node's NodeDB / contact list for the Nodes view (per-node JSON; fields are backend-specific),
    // plus counts for the status line.
    void nodesDiscovered(const QJsonArray& nodes, int total, int online);

    // Link/status: state is searching|connecting|connected|noperm; nodeName is the local node's
    // display name once known.
    void linkStateChanged(const QString& state, bool nodePresent, const QString& nodeName);
};

#endif // MESH_RADIO_H
