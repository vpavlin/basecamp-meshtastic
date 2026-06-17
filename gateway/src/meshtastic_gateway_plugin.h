#ifndef MESHTASTIC_GATEWAY_PLUGIN_H
#define MESHTASTIC_GATEWAY_PLUGIN_H

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include "meshtastic_gateway_interface.h"

class LogosAPI;
class LogosAPIClient;
class LogosObject;
class QSerialPort;

// Bridges a connected Meshtastic node's channels to/from Logos Messaging topics.
//
// STUB: the Meshtastic transport (serial framing + protobufs / BLE) is not yet implemented —
// getChannels() currently returns mock channels so the UI/UX can be built. The topic derivation
// (md5(name+psk), falling back to md5("idx:N")) is REAL, so the topics + bridging logic are final.
// To go live, replace loadStubChannels() with a real node connection that fills m_channels from the
// device's channel list, and wire bridgeChannel()/unbridge() to delivery + the mesh transport.
class MeshtasticGatewayPlugin : public QObject, public MeshtasticGatewayInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MeshtasticGatewayInterface_iid FILE "metadata.json")
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

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private:
    void loadStubChannels();                                       // TODO: real node read
    void seedStubMessages();                                       // TODO: real node read
    void seedStubNodes();                                          // TODO: real NodeDB read
    static QString deriveTopic(const QString& name, const QByteArray& psk, int index);
    static QString pskStatus(const QString& pskB64);               // none | default | custom
    int monotonicTs();                                             // stub clock (no Date in QML/here)
    void setChannelUnread(int channelIndex, int unread);           // mutate m_channels + emit

    // Relay / loop-prevention (see DATAFLOWS.md). Returns true if the message was bridged to the
    // channel's LM topic; false if the channel isn't relaying or the message is a suppressed echo.
    bool isRelaying(int channelIndex) const;
    QString topicFor(int channelIndex) const;
    int channelForTopic(const QString& topic) const;
    static QString fingerprint(int channelIndex, const QString& text);
    bool maybeRelay(int channelIndex, const QString& text, const QString& origin);

    // Logos Delivery (Messaging) integration — depend on delivery_module via Logos Core IPC.
    void initDelivery();                                     // deferred: connect, createNode, start
    void subscribeRelayTopics();                             // subscribe topics of relaying channels
    void publishToLM(const QString& topic, const QString& text);
    void onDeliveryMessage(const QVariantList& data);        // messageReceived: [hash,topic,b64,ts]

    // Meshtastic node over USB serial — speak the StreamAPI directly (QtSerialPort + protobuf),
    // in-process. Same data the stoa Python bridge reads. The radio decrypts on-air traffic, so
    // over serial we only ever handle plaintext (no crypto here).
    void openSerial();                                       // (re)connect + send want_config
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
    int m_deliveryTries = 0;                // bounded connect retries
    QSerialPort* m_serial = nullptr;        // USB link to the Meshtastic node
    QByteArray m_rxBuf;                      // StreamAPI byte accumulator (0x94 0xc3 framing)
    QJsonArray m_cfgChannels;               // channels gathered during the want_config burst
    QMap<quint32, QString> m_nodeNames;     // node num -> short name (for inbound message sender)
    quint32 m_myNum = 0;                     // our own node number
    QString m_pendingNodeName;              // our node's display name
    int m_cfgNodesTotal = 0;                // node DB counters (during config)
    int m_cfgNodesOnline = 0;
    quint32 m_wantConfigId = 0;             // nonce echoed back in config_complete_id
    int m_serialTries = 0;                  // bounded reconnect attempts
    QJsonArray m_channels;     // [{channelIndex,name,displayName,role,psk,pskStatus,topic,shareUrl,relaying,unread}]
    QMap<int, QJsonArray> m_messages;   // channelIndex -> [{id,from,text,ts,outgoing,origin,relayed,ackStatus,reactions}]
    QSet<QString> m_seen;               // dedup fingerprints (bridged-already)
    QStringList m_seenFifo;             // FIFO to bound m_seen (evict oldest past cap)
    int m_clock = 0;                    // stub timestamp counter
    int m_msgId = 0;                    // monotonic message id (for reactions / ack tracking)
    bool m_nodePresent = false;
    QString m_nodeName;
    int m_nodesTotal = 0;               // mesh nodes in the NodeDB (stub)
    int m_nodesOnline = 0;              // heard within the activity window (stub)
};

#endif
