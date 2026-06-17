#include "meshtastic_gateway_plugin.h"
#include "logos_api.h"
#include "logos_api_client.h"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QDateTime>
#include <QDebug>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

MeshtasticGatewayPlugin::MeshtasticGatewayPlugin(QObject* parent) : QObject(parent)
{
    // Dev/demo without a radio: MESHTASTIC_GATEWAY_STUB=1 keeps the seeded channels/messages/nodes.
    // Otherwise we start empty and the mesh_helper.py sidecar fills in real data (see initLogos).
    if (qEnvironmentVariableIsSet("MESHTASTIC_GATEWAY_STUB")) {
        loadStubChannels();
        seedStubNodes();
        seedStubMessages();
    }
}
MeshtasticGatewayPlugin::~MeshtasticGatewayPlugin() = default;

void MeshtasticGatewayPlugin::initLogos(LogosAPI* api)
{
    logosAPI = api;     // PluginInterface public field — SDK's QtProviderObject reads THIS to
                        // dispatch incoming IPC; without it every callMethod fails with
                        // "LogosAPI not available" and the UI gets null from getChannels()/status().
    m_logosAPI = api;
    emit eventResponse("nodeStatus", QVariantList() << m_nodePresent << m_nodeName);
    qInfo() << "meshtastic_gateway: init, node present" << m_nodePresent
            << "channels" << m_channels.size();
    // Defer delivery setup off the QRO reply thread (createNode/start are slow + async).
    QTimer::singleShot(0, this, [this]() { initDelivery(); });
    // Connect to the Meshtastic radio over USB serial unless we're in stub mode.
    if (!qEnvironmentVariableIsSet("MESHTASTIC_GATEWAY_STUB"))
        QTimer::singleShot(0, this, [this]() { openSerial(); });
}

// REAL derivation (final): only someone holding the channel name+psk can compute the topic, so it
// doubles as a membership-gated rendezvous. Empty name (default channel) -> hash the index.
QString MeshtasticGatewayPlugin::deriveTopic(const QString& name, const QByteArray& psk, int index)
{
    QByteArray id = name.isEmpty() ? ("idx:" + QByteArray::number(index))
                                   : (name.toUtf8() + psk);
    QString h = QString::fromLatin1(
        QCryptographicHash::hash(id, QCryptographicHash::Md5).toHex().left(16));
    return "/meshtastic/1/" + h + "/proto";
}

// Classify a channel's PSK the way Meshtastic clients badge it:
//   empty     -> "none"     (red, unencrypted)
//   "AQ=="    -> "default"  (yellow key, the well-known default LongFast key)
//   anything  -> "custom"   (green lock, a real shared secret)
QString MeshtasticGatewayPlugin::pskStatus(const QString& pskB64)
{
    if (pskB64.isEmpty()) return QStringLiteral("none");
    if (pskB64 == QStringLiteral("AQ==")) return QStringLiteral("default");
    return QStringLiteral("custom");
}

// TODO: replace with a real Meshtastic node connection (serial 0x94 0xc3 framing + protobufs, or
// BLE/TCP). On connect, read the device's Channel list and fill m_channels from it; construct each
// shareUrl from a single-channel ChannelSet protobuf -> base64url.
void MeshtasticGatewayPlugin::loadStubChannels()
{
    struct Stub { int idx; const char* name; const char* display; const char* role; const char* pskB64; bool relaying; int unread; };
    const Stub stubs[] = {
        { 0, "",        "LongFast", "PRIMARY",   "AQ==",                                         false, 5 },
        { 1, "stoa",    "stoa",     "SECONDARY", "1PG7OiApB1nwvP+rz05pAQ==",                     true,  2 },
        { 2, "vanlife", "vanlife",  "SECONDARY", "BvQAfHQ0o2VtNoT4kYjq3w0vJ9YqHk0n5z2cLm8rT4U=", false, 0 },
    };
    m_channels = QJsonArray();
    for (const auto& s : stubs) {
        const QByteArray psk = QByteArray::fromBase64(QByteArray(s.pskB64));
        QJsonObject c;
        c["channelIndex"] = s.idx;
        c["name"] = QString::fromUtf8(s.name);
        c["displayName"] = QString::fromUtf8(s.display);
        c["role"] = QString::fromUtf8(s.role);
        c["psk"] = QString::fromUtf8(s.pskB64);
        c["pskStatus"] = pskStatus(QString::fromUtf8(s.pskB64));
        c["topic"] = deriveTopic(QString::fromUtf8(s.name), psk, s.idx);
        // Stub share URL (the real one is a single-channel ChannelSet protobuf, base64url-encoded).
        c["shareUrl"] = QStringLiteral("https://meshtastic.org/e/#stub-")
                        + QString::fromUtf8(s.pskB64).left(10);
        c["relaying"] = s.relaying;
        c["unread"] = s.unread;     // UI badge; cleared via markRead() when opened
        m_channels.append(c);
    }
    m_nodePresent = true;                 // pretend a node is attached
    m_nodeName = "Heltec V3 (stub)";
}

QString MeshtasticGatewayPlugin::getChannels()
{
    return QString::fromUtf8(QJsonDocument(m_channels).toJson(QJsonDocument::Compact));
}

void MeshtasticGatewayPlugin::setRelay(int index, bool enabled)
{
    for (int i = 0; i < m_channels.size(); ++i) {
        QJsonObject c = m_channels[i].toObject();
        if (c["channelIndex"].toInt() == index) {
            c["relaying"] = enabled;
            m_channels[i] = c;
            const QString topic = c["topic"].toString();
            qInfo() << "meshtastic_gateway: relay" << (enabled ? "ON" : "off")
                    << "for channel" << index << "->" << topic;
            // Subscribe/unsubscribe the channel's LM topic so we start/stop receiving from it.
            if (m_deliveryReady && m_delivery) {
                m_delivery->invokeRemoteMethodAsync("delivery_module",
                    enabled ? "subscribe" : "unsubscribe", QVariant(topic), [](QVariant) {});
            }
            emit eventResponse("channelsChanged", QVariantList());
            return;
        }
    }
}

QString MeshtasticGatewayPlugin::status()
{
    QJsonObject o;
    o["nodePresent"] = m_nodePresent;
    o["nodeName"] = m_nodeName;
    o["channelCount"] = m_channels.size();
    o["nodesTotal"] = m_nodesTotal;     // mesh nodes known to the local node
    o["nodesOnline"] = m_nodesOnline;   // heard within the activity window
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// TODO: real NodeDB read — the Meshtastic node streams a NodeInfo list (num, user.longName/shortName,
// lastHeard, snr, battery, position). "online" = lastHeard within an activity window (~2h default).
void MeshtasticGatewayPlugin::seedStubNodes()
{
    m_nodesTotal = 7;
    m_nodesOnline = 4;
}

// --- Relay / loop-prevention (see DATAFLOWS.md) -------------------------

bool MeshtasticGatewayPlugin::isRelaying(int channelIndex) const
{
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        if (c["channelIndex"].toInt() == channelIndex) return c["relaying"].toBool();
    }
    return false;
}

QString MeshtasticGatewayPlugin::topicFor(int channelIndex) const
{
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        if (c["channelIndex"].toInt() == channelIndex) return c["topic"].toString();
    }
    return QString();
}

QString MeshtasticGatewayPlugin::fingerprint(int channelIndex, const QString& text)
{
    // Text-based fingerprint for the PoC. Production keys on source-assigned packet ids
    // (Meshtastic packet id / Waku message hash) — see DATAFLOWS.md "Robust identity".
    const QByteArray key = QByteArray::number(channelIndex) + "\n" + text.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex());
}

// The one rule that stops echo storms: if we've already bridged this fingerprint, it's an echo of
// our own relay coming back from the other network — suppress it. Otherwise record it (so the echo
// that WILL come back is pre-suppressed) and forward to the opposite network.
bool MeshtasticGatewayPlugin::maybeRelay(int channelIndex, const QString& text, const QString& origin)
{
    if (!isRelaying(channelIndex)) return false;

    const QString fp = fingerprint(channelIndex, text);
    if (m_seen.contains(fp)) {
        qInfo() << "meshtastic_gateway: dedup — suppressing" << origin
                << "echo on ch" << channelIndex << "(already bridged)";
        return false;
    }
    m_seen.insert(fp);
    m_seenFifo.append(fp);
    while (m_seenFifo.size() > 256) m_seen.remove(m_seenFifo.takeFirst());  // bound the set

    // maybeRelay is the LM-publish direction (local send, or future mesh-inbound). Inbound LM
    // messages take the onDeliveryMessage() path instead.
    const QString topic = topicFor(channelIndex);
    qInfo() << "meshtastic_gateway: relay" << origin << "->LM" << topic << ":" << text;
    publishToLM(topic, text);
    return true;
}

// --- Logos Delivery (Messaging) bridge ----------------------------------

void MeshtasticGatewayPlugin::initDelivery()
{
    if (!m_logosAPI) return;
    if (!m_delivery)                  m_delivery = m_logosAPI->getClient("delivery_module");
    if (!m_deliveryObj && m_delivery) m_deliveryObj = m_delivery->requestObject("delivery_module");

    if (!m_delivery || !m_deliveryObj) {
        if (++m_deliveryTries <= 60) {              // delivery_module can take a while to come up
            QTimer::singleShot(500, this, [this]() { initDelivery(); });
        } else {
            qWarning() << "meshtastic_gateway: delivery_module not reachable — relay disabled";
        }
        return;
    }

    // Subscribe to inbound LM messages first (event payload: [hash, contentTopic, payloadB64, ts]).
    m_delivery->onEvent(m_deliveryObj, "messageReceived",
        [this](const QString&, const QVariantList& data) { onDeliveryMessage(data); });

    // Edge node on the logos.dev cluster, then subscribe the relaying channels' topics.
    const QString cfg = QStringLiteral("{\"mode\":\"Edge\",\"preset\":\"logos.dev\"}");
    qInfo() << "meshtastic_gateway: delivery createNode…";
    m_delivery->invokeRemoteMethodAsync("delivery_module", "createNode", QVariant(cfg),
        [this](QVariant) {
            qInfo() << "meshtastic_gateway: delivery start…";
            m_delivery->invokeRemoteMethodAsync("delivery_module", "start", QVariantList(),
                [this](QVariant) {
                    m_deliveryReady = true;
                    qInfo() << "meshtastic_gateway: delivery node ready";
                    subscribeRelayTopics();
                });
        });
}

void MeshtasticGatewayPlugin::subscribeRelayTopics()
{
    if (!m_deliveryReady || !m_delivery) return;
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        if (!c["relaying"].toBool()) continue;
        const QString topic = c["topic"].toString();
        qInfo() << "meshtastic_gateway: subscribe" << topic;
        m_delivery->invokeRemoteMethodAsync("delivery_module", "subscribe",
                                             QVariant(topic), [](QVariant) {});
    }
}

void MeshtasticGatewayPlugin::publishToLM(const QString& topic, const QString& text)
{
    if (!m_deliveryReady || !m_delivery) {
        qWarning() << "meshtastic_gateway: delivery not ready — dropping publish to" << topic;
        return;
    }
    // delivery_module.send(contentTopic, payload) base64-encodes payload itself -> pass raw text.
    m_delivery->invokeRemoteMethodAsync("delivery_module", "send",
                                        QVariant(topic), QVariant(text), [](QVariant) {});
}

int MeshtasticGatewayPlugin::channelForTopic(const QString& topic) const
{
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        if (c["topic"].toString() == topic) return c["channelIndex"].toInt();
    }
    return -1;
}

// Inbound from Logos Messaging. data = [messageHash, contentTopic, payloadBase64, timestamp].
void MeshtasticGatewayPlugin::onDeliveryMessage(const QVariantList& data)
{
    if (data.size() < 3) return;
    const QString topic = data[1].toString();
    const QString text  = QString::fromUtf8(QByteArray::fromBase64(data[2].toString().toUtf8()));
    const int ch = channelForTopic(topic);
    if (ch < 0) return;                         // not one of our channels

    const QString fp = fingerprint(ch, text);
    if (m_seen.contains(fp)) {                  // our own publish echoing back — suppress (Flow A/C)
        qInfo() << "meshtastic_gateway: dedup — suppressing own LM echo on ch" << ch;
        return;
    }
    m_seen.insert(fp);
    m_seenFifo.append(fp);
    while (m_seenFifo.size() > 256) m_seen.remove(m_seenFifo.takeFirst());

    QJsonObject o;
    o["id"] = ++m_msgId;
    o["from"] = "Logos Messaging";
    o["text"] = text;
    o["ts"] = monotonicTs();
    o["outgoing"] = false;
    o["origin"] = "lm";
    o["relayed"] = true;
    o["ackStatus"] = QString();
    o["reactions"] = QJsonArray();
    m_messages[ch].append(o);
    emit eventResponse("messagesChanged", QVariantList() << ch);

    qInfo() << "meshtastic_gateway: relay LM->mesh ch" << ch << ":" << text;
    sendToMesh(ch, text);   // inject onto the LoRa channel (fingerprint already in `seen`)
}

// --- Meshtastic node over USB serial (StreamAPI) ------------------------

static constexpr unsigned char MT_START1 = 0x94;
static constexpr unsigned char MT_START2 = 0xC3;
static constexpr quint32 MT_BROADCAST = 0xFFFFFFFFu;

// Wrap a serialized ToRadio in a StreamAPI frame: 0x94 0xC3 <lenHi> <lenLo> <protobuf>.
static QByteArray frameToRadio(const std::string& buf)
{
    QByteArray f;
    f.append(char(MT_START1));
    f.append(char(MT_START2));
    f.append(char((buf.size() >> 8) & 0xFF));
    f.append(char(buf.size() & 0xFF));
    f.append(buf.data(), int(buf.size()));
    return f;
}

void MeshtasticGatewayPlugin::openSerial()
{
    if (m_serial && m_serial->isOpen()) return;

    // Port: $MESHTASTIC_DEV, else autodetect a likely Meshtastic USB device (by USB vendor/desc).
    QString port = qEnvironmentVariable("MESHTASTIC_DEV");
    if (port.isEmpty()) {
        for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
            const QString d = (info.manufacturer() + " " + info.description()).toLower();
            const quint16 vid = info.vendorIdentifier();
            if (vid == 0x303a || vid == 0x10c4 || vid == 0x1a86 ||   // Espressif / SiLabs / CH34x
                d.contains("heltec") || d.contains("rak") || d.contains("meshtastic")) {
                port = info.systemLocation();
                break;
            }
        }
    }
    if (port.isEmpty()) {
        if (m_serialTries++ % 6 == 0)
            qInfo() << "meshtastic_gateway: no Meshtastic serial device found — waiting";
        m_nodePresent = false;
        emit eventResponse("nodeStatus", QVariantList() << m_nodePresent << m_nodeName);
        QTimer::singleShot(5000, this, [this]() { openSerial(); });
        return;
    }

    if (!m_serial) {
        m_serial = new QSerialPort(this);
        connect(m_serial, &QSerialPort::readyRead, this, [this]() { onSerialReadyRead(); });
        connect(m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError) {
                qWarning() << "meshtastic_gateway: serial error" << e << "— reconnecting";
                if (m_serial->isOpen()) m_serial->close();
                m_nodePresent = false;
                emit eventResponse("nodeStatus", QVariantList() << m_nodePresent << m_nodeName);
                QTimer::singleShot(3000, this, [this]() { openSerial(); });
            }
        });
    }
    m_serial->setPortName(port);
    m_serial->setBaudRate(QSerialPort::Baud115200);
    if (!m_serial->open(QIODevice::ReadWrite)) {
        qWarning() << "meshtastic_gateway: cannot open" << port << "—" << m_serial->errorString();
        QTimer::singleShot(3000, this, [this]() { openSerial(); });
        return;
    }
    qInfo() << "meshtastic_gateway: serial open on" << port << "— requesting config";
    m_rxBuf.clear();
    m_cfgChannels = QJsonArray();
    m_cfgNodesTotal = m_cfgNodesOnline = 0;

    // want_config -> radio replies with MyNodeInfo + NodeInfos + Channels + config, then
    // config_complete_id echoing this nonce.
    m_wantConfigId = 0x6d657368u;   // "mesh"
    meshtastic::ToRadio toRadio;
    toRadio.set_want_config_id(m_wantConfigId);
    m_serial->write(frameToRadio(toRadio.SerializeAsString()));
}

void MeshtasticGatewayPlugin::onSerialReadyRead()
{
    m_rxBuf += m_serial->readAll();
    while (true) {
        int start = -1;
        for (int i = 0; i + 1 < m_rxBuf.size(); ++i) {
            if ((unsigned char)m_rxBuf[i] == MT_START1 && (unsigned char)m_rxBuf[i + 1] == MT_START2) {
                start = i;
                break;
            }
        }
        if (start < 0) { if (m_rxBuf.size() > 8192) m_rxBuf.clear(); return; }  // resync
        if (start > 0) m_rxBuf.remove(0, start);                 // drop noise before the header
        if (m_rxBuf.size() < 4) return;                          // need header + length
        const int len = ((unsigned char)m_rxBuf[2] << 8) | (unsigned char)m_rxBuf[3];
        if (m_rxBuf.size() < 4 + len) return;                    // wait for the body
        const QByteArray payload = m_rxBuf.mid(4, len);
        m_rxBuf.remove(0, 4 + len);
        handleFromRadio(payload);
    }
}

void MeshtasticGatewayPlugin::handleFromRadio(const QByteArray& payload)
{
    meshtastic::FromRadio fr;
    if (!fr.ParseFromArray(payload.constData(), payload.size())) return;

    switch (fr.payload_variant_case()) {
    case meshtastic::FromRadio::kMyInfo:
        m_myNum = fr.my_info().my_node_num();
        break;
    case meshtastic::FromRadio::kNodeInfo: {
        const meshtastic::NodeInfo& n = fr.node_info();
        m_cfgNodesTotal++;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (n.last_heard() && now - qint64(n.last_heard()) < 7200) m_cfgNodesOnline++;
        if (n.has_user()) {
            const QString shortName = QString::fromStdString(n.user().short_name());
            const QString longName = QString::fromStdString(n.user().long_name());
            m_nodeNames[n.num()] = shortName.isEmpty() ? longName : shortName;
            if (n.num() == m_myNum) m_pendingNodeName = longName.isEmpty() ? shortName : longName;
        }
        break;
    }
    case meshtastic::FromRadio::kChannel: {
        const meshtastic::Channel& c = fr.channel();
        if (c.role() != meshtastic::Channel_Role_DISABLED) {
            const std::string& psk = c.settings().psk();
            QJsonObject o;
            o["channelIndex"] = c.index();
            o["name"] = QString::fromStdString(c.settings().name());
            o["psk"] = QString::fromLatin1(QByteArray(psk.data(), int(psk.size())).toBase64());
            o["role"] = c.role() == meshtastic::Channel_Role_PRIMARY ? "PRIMARY" : "SECONDARY";
            m_cfgChannels.append(o);
        }
        break;
    }
    case meshtastic::FromRadio::kConfigCompleteId:
        if (fr.config_complete_id() == m_wantConfigId) finalizeConfig();
        break;
    case meshtastic::FromRadio::kPacket: {
        const meshtastic::MeshPacket& p = fr.packet();
        if (p.has_decoded() && p.decoded().portnum() == meshtastic::TEXT_MESSAGE_APP) {
            const std::string& pl = p.decoded().payload();
            const QString text = QString::fromUtf8(pl.data(), int(pl.size()));
            const QString from = m_nodeNames.value(
                p.from(), QStringLiteral("!%1").arg(p.from(), 8, 16, QChar('0')));
            onMeshMessage(int(p.channel()), from, text);
        }
        break;
    }
    default:
        break;
    }
}

void MeshtasticGatewayPlugin::finalizeConfig()
{
    m_nodePresent = true;
    if (!m_pendingNodeName.isEmpty()) m_nodeName = m_pendingNodeName;
    m_nodesTotal = m_cfgNodesTotal;
    m_nodesOnline = m_cfgNodesOnline;
    qInfo() << "meshtastic_gateway: radio config complete —" << m_cfgChannels.size()
            << "channels," << m_nodesTotal << "nodes (" << m_nodesOnline << "online)";
    rebuildChannelsFromMesh(m_cfgChannels);     // emits channelsChanged + (re)subscribes LM topics
    emit eventResponse("nodeStatus", QVariantList() << m_nodePresent << m_nodeName);
}

void MeshtasticGatewayPlugin::rebuildChannelsFromMesh(const QJsonArray& chans)
{
    // Preserve gateway-side per-channel state (relay toggle + unread) across refreshes.
    QMap<int, bool> wasRelaying;
    QMap<int, int> hadUnread;
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        wasRelaying[c["channelIndex"].toInt()] = c["relaying"].toBool();
        hadUnread[c["channelIndex"].toInt()] = c["unread"].toInt();
    }

    QJsonArray rebuilt;
    for (const auto& v : chans) {
        const QJsonObject m = v.toObject();
        const int idx = m["channelIndex"].toInt();
        const QString name = m["name"].toString();
        const QString role = m["role"].toString();
        const QString pskB64 = m["psk"].toString();
        const QByteArray psk = QByteArray::fromBase64(pskB64.toUtf8());

        QJsonObject c;
        c["channelIndex"] = idx;
        c["name"] = name;
        c["displayName"] = name.isEmpty() ? (role == "PRIMARY" ? QStringLiteral("Primary")
                                                               : QStringLiteral("ch%1").arg(idx))
                                          : name;
        c["role"] = role;
        c["psk"] = pskB64;
        c["pskStatus"] = pskStatus(pskB64);
        c["topic"] = deriveTopic(name, psk, idx);          // real name+psk -> real LM topic
        c["relaying"] = wasRelaying.value(idx, false);
        c["unread"] = hadUnread.value(idx, 0);
        rebuilt.append(c);
    }
    m_channels = rebuilt;
    emit eventResponse("channelsChanged", QVariantList());
    if (m_deliveryReady) subscribeRelayTopics();           // topics may have changed
}

// Inbound text heard on the LoRa mesh.
void MeshtasticGatewayPlugin::onMeshMessage(int channelIndex, const QString& from, const QString& text)
{
    const QString fp = fingerprint(channelIndex, text);
    if (m_seen.contains(fp)) {           // our own injection echoing off the mesh — suppress
        qInfo() << "meshtastic_gateway: dedup — suppressing own mesh echo on ch" << channelIndex;
        return;
    }
    QJsonObject o;
    o["id"] = ++m_msgId;
    o["from"] = from.isEmpty() ? QStringLiteral("mesh") : from;
    o["text"] = text;
    o["ts"] = monotonicTs();
    o["outgoing"] = false;
    o["origin"] = "mesh";
    o["relayed"] = isRelaying(channelIndex);
    o["ackStatus"] = QString();
    o["reactions"] = QJsonArray();
    m_messages[channelIndex].append(o);
    emit eventResponse("messagesChanged", QVariantList() << channelIndex);

    maybeRelay(channelIndex, text, "mesh");   // bridge to LM if relaying (records fingerprint)
}

void MeshtasticGatewayPlugin::sendToMesh(int channelIndex, const QString& text)
{
    if (!m_serial || !m_serial->isOpen()) {
        qWarning() << "meshtastic_gateway: serial not open — mesh TX dropped on ch" << channelIndex;
        return;
    }
    meshtastic::ToRadio toRadio;
    meshtastic::MeshPacket* p = toRadio.mutable_packet();
    p->set_to(MT_BROADCAST);
    p->set_channel(quint32(channelIndex));
    meshtastic::Data* d = p->mutable_decoded();
    d->set_portnum(meshtastic::TEXT_MESSAGE_APP);
    const QByteArray utf8 = text.toUtf8();
    d->set_payload(utf8.constData(), utf8.size());
    m_serial->write(frameToRadio(toRadio.SerializeAsString()));
}

// --- Chat ---------------------------------------------------------------

int MeshtasticGatewayPlugin::monotonicTs()
{
    // Stub clock: QML's JS engine and a deterministic build both lack a wall clock here, and we
    // only need ordering for the demo. Real impl uses the Meshtastic packet rxTime.
    return ++m_clock;
}

// TODO: real node read — seed each channel with a little plausible history so the chat view has
// something to show. Replace with the live message stream from the Meshtastic node.
void MeshtasticGatewayPlugin::seedStubMessages()
{
    struct M { int ch; const char* from; const char* text; bool out; const char* origin; const char* react; };
    const M seed[] = {
        { 1, "stoa-base", "Camp set up at the lake \xF0\x9F\x8F\x95",        false, "mesh",  "\xF0\x9F\x91\x8D" },  // 👍
        { 1, "laptop",    "Heading out from the office",                     false, "lm",    "" },
        { 1, "me",        "Nice! ETA 20 min",                                true,  "local", "" },
        { 1, "van-2",     "Bringing firewood \xF0\x9F\x94\xA5",              false, "mesh",  "" },
        { 2, "vanlife",   "Anyone near exit 42?",                            false, "mesh",  "" },
        { 2, "me",        "Yep, pulling in now",                             true,  "local", "" },
        { 0, "LongFast",  "Region net online",                              false, "mesh",  "" },
    };
    for (const auto& m : seed) {
        QJsonObject o;
        o["id"] = ++m_msgId;
        o["from"] = QString::fromUtf8(m.from);
        o["text"] = QString::fromUtf8(m.text);
        o["ts"] = monotonicTs();
        o["outgoing"] = m.out;
        o["origin"] = QString::fromUtf8(m.origin);
        // On a relaying channel every message crossed the bridge; the UI shows a relay tick
        // (origin=="lm" renders as "from Logos Messaging", otherwise "relayed to Logos Messaging").
        o["relayed"] = isRelaying(m.ch);
        o["ackStatus"] = m.out ? QStringLiteral("delivered") : QString();
        QJsonArray reactions;
        if (m.react && *m.react) {
            QJsonObject r; r["emoji"] = QString::fromUtf8(m.react); r["count"] = 1;
            reactions.append(r);
        }
        o["reactions"] = reactions;
        m_messages[m.ch].append(o);
    }
}

QString MeshtasticGatewayPlugin::getMessages(int channelIndex)
{
    const QJsonArray arr = m_messages.value(channelIndex);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void MeshtasticGatewayPlugin::sendMessage(int channelIndex, const QString& text)
{
    const QString body = text.trimmed();
    if (body.isEmpty()) return;

    // Bridge first — records the fingerprint BEFORE any echo can return (DATAFLOWS.md Flow C).
    const bool relayed = maybeRelay(channelIndex, body, "local");

    const int id = ++m_msgId;
    QJsonObject o;
    o["id"] = id;
    o["from"] = "me";
    o["text"] = body;
    o["ts"] = monotonicTs();
    o["outgoing"] = true;
    o["origin"] = "local";
    o["relayed"] = relayed;
    o["ackStatus"] = QStringLiteral("enroute");   // mesh delivery isn't instant; cycle to delivered
    o["reactions"] = QJsonArray();
    m_messages[channelIndex].append(o);
    // TODO real: mesh.send(channelIndex, body);  (maybeRelay() already handled the LM publish)

    emit eventResponse("messagesChanged", QVariantList() << channelIndex);

    // Stub: simulate the mesh ack landing ~1.4s later so the status icon visibly cycles
    // enroute -> delivered. Real impl flips this on the node's ACK packet for this packet id.
    QTimer::singleShot(1400, this, [this, channelIndex, id]() {
        QJsonArray& arr = m_messages[channelIndex];
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject m = arr[i].toObject();
            if (m["id"].toInt() == id) {
                m["ackStatus"] = QStringLiteral("delivered");
                arr[i] = m;
                emit eventResponse("messagesChanged", QVariantList() << channelIndex);
                break;
            }
        }
    });

    // Loop-prevention is now real: maybeRelay() recorded the fingerprint and published to LM; when
    // delivery echoes the message back to our own subscription, onDeliveryMessage() finds the
    // fingerprint in `seen` and suppresses it (see "dedup — suppressing own LM echo" in the log).
}

void MeshtasticGatewayPlugin::setChannelUnread(int channelIndex, int unread)
{
    for (int i = 0; i < m_channels.size(); ++i) {
        QJsonObject c = m_channels[i].toObject();
        if (c["channelIndex"].toInt() == channelIndex) {
            if (c["unread"].toInt() == unread) return;
            c["unread"] = unread;
            m_channels[i] = c;
            emit eventResponse("channelsChanged", QVariantList());
            return;
        }
    }
}

void MeshtasticGatewayPlugin::markRead(int channelIndex)
{
    setChannelUnread(channelIndex, 0);
}

void MeshtasticGatewayPlugin::addReaction(int channelIndex, int messageId, const QString& emoji)
{
    if (emoji.isEmpty() || !m_messages.contains(channelIndex)) return;
    QJsonArray& arr = m_messages[channelIndex];
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject m = arr[i].toObject();
        if (m["id"].toInt() != messageId) continue;

        QJsonArray reactions = m["reactions"].toArray();
        bool found = false;
        for (int j = 0; j < reactions.size(); ++j) {
            QJsonObject r = reactions[j].toObject();
            if (r["emoji"].toString() == emoji) {
                r["count"] = r["count"].toInt() + 1;
                reactions[j] = r;
                found = true;
                break;
            }
        }
        if (!found) {
            QJsonObject r; r["emoji"] = emoji; r["count"] = 1;
            reactions.append(r);
        }
        m["reactions"] = reactions;
        arr[i] = m;
        emit eventResponse("messagesChanged", QVariantList() << channelIndex);
        return;
    }
}
