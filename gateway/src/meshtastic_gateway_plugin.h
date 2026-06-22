#ifndef MESHTASTIC_GATEWAY_PLUGIN_H
#define MESHTASTIC_GATEWAY_PLUGIN_H

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QSqlDatabase>
#include "meshtastic_gateway_interface.h"

class LogosAPI;
class LogosAPIClient;
class LogosObject;
class QSerialPort;

// Bridges a connected Meshtastic node's LoRa channels to/from Logos Messaging topics.
//
// Mesh side: native StreamAPI over USB serial (QtSerialPort + meshtastic protobufs) — discovers the
// node, reads its channel list + NodeDB, sends/receives text, and writes config via AdminMessage.
// LM side: depends on delivery_module via Logos Core IPC. Each channel maps to a content topic
// (md5(name+psk)[:16], or md5("idx:N") when unnamed); a per-channel opt-in relay bridges the two.
// Chat history + relay prefs + settings persist in SQLite. The UI is fully signal-driven (events).
class MeshtasticGatewayPlugin : public QObject, public MeshtasticGatewayInterface
{
    Q_OBJECT
    // Embedded metadata uses metadata.embedded.json (NOT metadata.json) on purpose. Basecamp's
    // dependency resolver reads the deps baked into this .so (Qt plugin metadata), so the embedded
    // copy must declare dependencies:["delivery_module"] to make Basecamp auto-load delivery_module.
    // The build-time metadata.json keeps dependencies:[] — listing the dep there makes the SDK
    // codegen #include a delivery_module_api.h that our events-only delivery clone doesn't generate,
    // which breaks the build. Keep both files in sync except for the dependencies array.
    Q_PLUGIN_METADATA(IID MeshtasticGatewayInterface_iid FILE "metadata.embedded.json")
    Q_INTERFACES(MeshtasticGatewayInterface PluginInterface)

public:
    explicit MeshtasticGatewayPlugin(QObject* parent = nullptr);
    ~MeshtasticGatewayPlugin() override;

    QString name() const override { return "meshtastic_gateway"; }
    QString version() const override { return "0.1.0"; }
    Q_INVOKABLE void initLogos(LogosAPI* api);   // Q_INVOKABLE, not override (SDK convention)

    Q_INVOKABLE QString getChannels() override;
    Q_INVOKABLE void setRelay(int index, bool enabled) override;
    Q_INVOKABLE QString status() override;
    Q_INVOKABLE QString getMessages(int channelIndex) override;
    Q_INVOKABLE void sendMessage(int channelIndex, const QString& text) override;
    Q_INVOKABLE void markRead(int channelIndex) override;
    Q_INVOKABLE void addReaction(int channelIndex, int messageId, const QString& emoji) override;

    // Push API (signal-driven UI — see DATAFLOWS.md "UI surfacing"). The UI never pulls data with a
    // blocking callModule; it fires these and renders from the resulting events. requestSnapshot()
    // re-pushes status+channels on plugin open; requestMessages() pushes one channel's history.
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void requestMessages(int channelIndex);
    Q_INVOKABLE void requestNodes();
    Q_INVOKABLE void requestSettings();                      // push current settings (settingsChanged)
    Q_INVOKABLE void setSetting(const QString& key, const QString& value);  // persist + apply + re-emit
    Q_INVOKABLE void setOwner(const QString& longName, const QString& shortName);  // write node name to radio
    Q_INVOKABLE void createChannel(const QString& name, const QString& pskB64);     // add a secondary channel
    Q_INVOKABLE void deleteChannel(int index);                                      // disable a channel (not 0)

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private:
    static QString deriveTopic(const QString& name, const QByteArray& psk, int index);
    static QString channelShareUrl(const QString& name, const QByteArray& psk);  // meshtastic.org/e/# URL

    // SQLite persistence — messages + relay prefs survive restarts (keyed by topic, not channel index,
    // so they're stable across channel reordering/reconnects). See DATAFLOWS.md.
    void openDb();                                          // open + create schema; restore id/clock
    void persistMessage(int channelIndex, const QJsonObject& m);   // INSERT OR REPLACE one message
    QJsonArray loadMessages(const QString& topic, int limit);      // last N messages for a topic, oldest→newest
    void saveRelayPref(const QString& topic, bool relaying);
    bool loadRelayPref(const QString& topic, bool fallback);       // persisted relay toggle (or fallback)
    static QString pskStatus(const QString& pskB64);               // none | default | custom
    int monotonicTs();                                             // persisted monotonic ordering counter
    void setChannelUnread(int channelIndex, int unread);           // mutate m_channels + emit

    // Event emitters — every event carries its FULL state payload so the UI renders from the event
    // alone and never has to make a (blocking) callModule to pull data back. data[0] is JSON.
    void emitChannels();                  // channelsChanged -> [channelsJSON]
    void emitStatus();                    // nodeStatus      -> [statusJSON]
    void emitMessages(int channelIndex);  // messagesChanged -> [channelIndex, messagesJSON]
    void emitNodes();                     // nodesChanged    -> [nodesJSON] (full per-node NodeDB)
    void scheduleNodesEmit();             // coalesce bursts of live NodeInfo into one emit
    void emitSettings();                  // settingsChanged -> [settingsJSON]
    void loadSettings();                  // read settings table into members (defaults if absent)

    // Relay / loop-prevention (see DATAFLOWS.md). Returns true if the message was bridged to the
    // channel's LM topic; false if the channel isn't relaying or the message is a suppressed echo.
    bool isRelaying(int channelIndex) const;
    QString topicFor(int channelIndex) const;
    int channelForTopic(const QString& topic) const;
    static QString fingerprint(int channelIndex, const QString& text);
    bool maybeRelay(int channelIndex, const QString& text, const QString& origin);

    // Logos Delivery (Messaging) integration — depend on delivery_module via Logos Core IPC.
    void initDelivery();                                     // deferred: connect, createNode, start
    void setDeliveryState(const QString& s);                 // update + emitStatus only when changed
    void subscribeRelayTopics();                             // subscribe topics of relaying channels
    void publishToLM(const QString& topic, const QString& text);
    void onDeliveryMessage(const QVariantList& data);        // messageReceived: [hash,topic,b64,ts]

    // Meshtastic node over USB serial — speak the StreamAPI directly (QtSerialPort + protobuf),
    // in-process. Same data the stoa Python bridge reads. The radio decrypts on-air traffic, so
    // over serial we only ever handle plaintext (no crypto here).
    void openSerial();                                       // (re)connect + send want_config
    void sendWantConfig();                                   // (re)send want_config until config_complete
    void onSerialReadyRead();                                // frame parser: 0x94 0xc3 <len> <pb>
    void handleFromRadio(const QByteArray& payload);         // dispatch one FromRadio packet
    void finalizeConfig();                                   // config_complete -> publish channels/nodes
    void rebuildChannelsFromMesh(const QJsonArray& chans);   // real name+psk -> real LM topics
    void onMeshMessage(int channelIndex, const QString& from, const QString& text);  // inbound LoRa
    void sendToMesh(int channelIndex, const QString& text);  // frame a ToRadio{MeshPacket text}

    LogosAPI* m_logosAPI = nullptr;
    LogosAPIClient* m_delivery = nullptr;   // delivery_module IPC client
    LogosObject* m_deliveryObj = nullptr;   // remote object handle for event subscription
    bool m_deliveryReady = false;           // node created + started
    QString m_deliveryState = "down";       // down | connecting | ready (Logos Messaging indicator)
    int m_deliveryTries = 0;                // bounded connect retries
    QSerialPort* m_serial = nullptr;        // USB link to the Meshtastic node
    QByteArray m_rxBuf;                      // StreamAPI byte accumulator (0x94 0xc3 framing)
    QJsonArray m_cfgChannels;               // channels gathered during the want_config burst
    QMap<quint32, QString> m_nodeNames;     // node num -> short name (for inbound message sender)
    QMap<quint32, QJsonObject> m_nodes;     // node num -> full NodeInfo JSON (for the Nodes view)
    quint32 m_myNum = 0;                     // our own node number
    QString m_pendingNodeName;              // our node's display name
    QString m_modemPreset;                  // LoRa modem preset (names a blank primary channel)
    int m_cfgNodesTotal = 0;                // node DB counters (during config)
    int m_cfgNodesOnline = 0;
    quint32 m_wantConfigId = 0;             // nonce echoed back in config_complete_id
    bool m_configComplete = false;          // config burst finished (stops want_config resend)
    int m_wantConfigTries = 0;              // bounded want_config resends (lost-on-open recovery)
    int m_serialTries = 0;                  // bounded reconnect attempts
    QJsonArray m_channels;     // [{channelIndex,name,displayName,role,psk,pskStatus,topic,shareUrl,relaying,unread}]
    QMap<int, QJsonArray> m_messages;   // channelIndex -> [{id,from,text,ts,outgoing,origin,relayed,ackStatus,reactions}]
    QSet<QString> m_seen;               // dedup fingerprints (bridged-already)
    QStringList m_seenFifo;             // FIFO to bound m_seen (evict oldest past cap)
    int m_clock = 0;                    // monotonic ordering counter (persisted; continues across restarts)
    int m_msgId = 0;                    // monotonic message id (for reactions / ack tracking; persisted)
    QSqlDatabase m_db;                  // SQLite store (named connection "meshtastic_gateway")
    static constexpr int kMsgLoadLimit = 500;   // messages loaded into memory per channel (DB keeps all)

    // App-behavior settings (persisted in the settings table; see loadSettings/setSetting).
    int m_onlineWindowSec = 7200;       // lastHeard within this window -> node "online" (default 2h)
    int m_maxMsgsPerChannel = 0;        // DB retention cap per channel; 0 = unlimited
    QString m_distanceUnit = "km";      // "km" | "mi" — UI display only
    bool m_nodePresent = false;
    QString m_linkState = "searching";  // searching | connecting | connected (UI status dot)
    bool m_nodesEmitPending = false;    // debounce flag for live nodesChanged emits
    QString m_nodeName;
    int m_nodesTotal = 0;               // mesh nodes in the NodeDB
    int m_nodesOnline = 0;              // heard within the activity window (m_onlineWindowSec)
};

#endif
