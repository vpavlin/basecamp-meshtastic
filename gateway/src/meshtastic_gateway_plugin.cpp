#include "meshtastic_gateway_plugin.h"
#include "logos_api.h"
#include "logos_api_client.h"

#include <cmath>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/apponly.pb.h"
#include <QRandomGenerator>

MeshtasticGatewayPlugin::MeshtasticGatewayPlugin(QObject* parent) : QObject(parent) {}
MeshtasticGatewayPlugin::~MeshtasticGatewayPlugin() = default;

void MeshtasticGatewayPlugin::initLogos(LogosAPI* api)
{
    logosAPI = api;     // PluginInterface public field — SDK's QtProviderObject reads THIS to
                        // dispatch incoming IPC; without it every callMethod fails with
                        // "LogosAPI not available" and the UI gets null from getChannels()/status().
    m_logosAPI = api;
    openDb();           // restore persisted messages/relay prefs before we connect to the radio
    emitStatus();
    qInfo() << "meshtastic_gateway: init, node present" << m_nodePresent
            << "channels" << m_channels.size();
    // Defer delivery setup off the QRO reply thread (createNode/start are slow + async).
    QTimer::singleShot(0, this, [this]() { initDelivery(); });
    // Connect to the Meshtastic radio over USB serial.
    QTimer::singleShot(0, this, [this]() { openSerial(); });
}

// REAL derivation (final): only someone holding the channel name+psk can compute the topic, so it
// doubles as a membership-gated rendezvous. Empty name (default channel) -> hash the index.
// "MEDIUM_FAST" -> "MediumFast" (Meshtastic shows a blank primary channel as its modem preset name)
static QString prettyPreset(const QString& e)
{
    QString out;
    const QStringList parts = e.split('_', Qt::SkipEmptyParts);
    for (const QString& w : parts) out += w.left(1).toUpper() + w.mid(1).toLower();
    return out;
}

// Meshtastic share URL: base64url(ChannelSet{ one ChannelSettings }) → https://meshtastic.org/e/#…
// A device that opens this adds the channel (name + psk). Single-channel share, no lora_config.
QString MeshtasticGatewayPlugin::channelShareUrl(const QString& name, const QByteArray& psk)
{
    meshtastic::ChannelSet set;
    meshtastic::ChannelSettings* s = set.add_settings();
    s->set_name(name.toStdString());
    s->set_psk(psk.constData(), psk.size());
    const std::string buf = set.SerializeAsString();
    const QByteArray b64 = QByteArray(buf.data(), int(buf.size()))
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return "https://meshtastic.org/e/#" + QString::fromLatin1(b64);
}

QString MeshtasticGatewayPlugin::deriveTopic(const QString& name, const QByteArray& psk, int index)
{
    QByteArray id = name.isEmpty() ? ("idx:" + QByteArray::number(index))
                                   : (name.toUtf8() + psk);
    QString h = QString::fromLatin1(
        QCryptographicHash::hash(id, QCryptographicHash::Md5).toHex().left(16));
    return "/meshtastic/1/" + h + "/proto";
}

// --- SQLite persistence ------------------------------------------------
// One file under the app data dir; messages + relay prefs keyed by topic (stable across reconnects).

void MeshtasticGatewayPlugin::openDb()
{
    if (m_db.isOpen()) return;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.local/share";
    dir += "/meshtastic_gateway";
    QDir().mkpath(dir);
    const QString path = dir + "/gateway.db";

    m_db = QSqlDatabase::addDatabase("QSQLITE", "meshtastic_gateway");
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        qWarning() << "meshtastic_gateway: cannot open DB" << path << "-" << m_db.lastError().text();
        return;
    }
    QSqlQuery q(m_db);
    q.exec("CREATE TABLE IF NOT EXISTS messages ("
           "topic TEXT NOT NULL, msgid INTEGER NOT NULL, sender TEXT, text TEXT, ts INTEGER,"
           "outgoing INTEGER, origin TEXT, relayed INTEGER, ackStatus TEXT, reactions TEXT,"
           "PRIMARY KEY(topic, msgid))");
    q.exec("CREATE INDEX IF NOT EXISTS idx_messages_topic_ts ON messages(topic, ts)");
    q.exec("CREATE TABLE IF NOT EXISTS channel_prefs (topic TEXT PRIMARY KEY, relaying INTEGER NOT NULL DEFAULT 0)");
    q.exec("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)");

    // Continue the id/ordering counters past anything already stored so we never collide on restart.
    if (q.exec("SELECT COALESCE(MAX(msgid),0), COALESCE(MAX(ts),0) FROM messages") && q.next()) {
        m_msgId = q.value(0).toInt();
        m_clock = q.value(1).toInt();
    }
    loadSettings();
    qInfo() << "meshtastic_gateway: DB open" << path << "— resume msgId" << m_msgId << "clock" << m_clock;
}

void MeshtasticGatewayPlugin::persistMessage(int channelIndex, const QJsonObject& m)
{
    if (!m_db.isOpen()) return;
    const QString topic = topicFor(channelIndex);
    if (topic.isEmpty()) return;                       // no stable key yet (pre-config) — skip
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO messages"
              "(topic,msgid,sender,text,ts,outgoing,origin,relayed,ackStatus,reactions)"
              " VALUES(?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(topic);
    q.addBindValue(m.value("id").toInt());
    q.addBindValue(m.value("from").toString());
    q.addBindValue(m.value("text").toString());
    q.addBindValue(m.value("ts").toInt());
    q.addBindValue(m.value("outgoing").toBool() ? 1 : 0);
    q.addBindValue(m.value("origin").toString());
    q.addBindValue(m.value("relayed").toBool() ? 1 : 0);
    q.addBindValue(m.value("ackStatus").toString());
    q.addBindValue(QString::fromUtf8(QJsonDocument(m.value("reactions").toArray()).toJson(QJsonDocument::Compact)));
    if (!q.exec()) { qWarning() << "meshtastic_gateway: persistMessage failed:" << q.lastError().text(); return; }

    // Retention: prune oldest rows for this topic beyond the cap (0 = unlimited).
    if (m_maxMsgsPerChannel > 0) {
        QSqlQuery p(m_db);
        p.prepare("DELETE FROM messages WHERE topic=? AND msgid NOT IN "
                  "(SELECT msgid FROM messages WHERE topic=? ORDER BY ts DESC, msgid DESC LIMIT ?)");
        p.addBindValue(topic);
        p.addBindValue(topic);
        p.addBindValue(m_maxMsgsPerChannel);
        p.exec();
    }
}

QJsonArray MeshtasticGatewayPlugin::loadMessages(const QString& topic, int limit)
{
    QJsonArray arr;
    if (!m_db.isOpen() || topic.isEmpty()) return arr;
    QSqlQuery q(m_db);
    // newest `limit` rows, then return oldest→newest for the chat view.
    q.prepare("SELECT msgid,sender,text,ts,outgoing,origin,relayed,ackStatus,reactions FROM messages"
              " WHERE topic=? ORDER BY ts DESC, msgid DESC LIMIT ?");
    q.addBindValue(topic);
    q.addBindValue(limit);
    if (!q.exec()) { qWarning() << "meshtastic_gateway: loadMessages failed:" << q.lastError().text(); return arr; }
    QList<QJsonObject> rows;
    while (q.next()) {
        QJsonObject o;
        o["id"] = q.value(0).toInt();
        o["from"] = q.value(1).toString();
        o["text"] = q.value(2).toString();
        o["ts"] = q.value(3).toInt();
        o["outgoing"] = q.value(4).toInt() != 0;
        o["origin"] = q.value(5).toString();
        o["relayed"] = q.value(6).toInt() != 0;
        o["ackStatus"] = q.value(7).toString();
        o["reactions"] = QJsonDocument::fromJson(q.value(8).toString().toUtf8()).array();
        rows.append(o);
    }
    for (int i = rows.size() - 1; i >= 0; --i) arr.append(rows[i]);   // reverse to oldest→newest
    return arr;
}

void MeshtasticGatewayPlugin::saveRelayPref(const QString& topic, bool relaying)
{
    if (!m_db.isOpen() || topic.isEmpty()) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO channel_prefs(topic,relaying) VALUES(?,?)");
    q.addBindValue(topic);
    q.addBindValue(relaying ? 1 : 0);
    q.exec();
}

bool MeshtasticGatewayPlugin::loadRelayPref(const QString& topic, bool fallback)
{
    if (!m_db.isOpen() || topic.isEmpty()) return fallback;
    QSqlQuery q(m_db);
    q.prepare("SELECT relaying FROM channel_prefs WHERE topic=?");
    q.addBindValue(topic);
    if (q.exec() && q.next()) return q.value(0).toInt() != 0;
    return fallback;
}

void MeshtasticGatewayPlugin::loadSettings()
{
    if (!m_db.isOpen()) return;
    QSqlQuery q(m_db);
    if (!q.exec("SELECT key,value FROM settings")) return;
    while (q.next()) {
        const QString k = q.value(0).toString(), v = q.value(1).toString();
        if (k == "onlineWindowSec")      m_onlineWindowSec   = v.toInt();
        else if (k == "maxMsgsPerChannel") m_maxMsgsPerChannel = v.toInt();
        else if (k == "distanceUnit")    m_distanceUnit      = v;
    }
}

void MeshtasticGatewayPlugin::emitSettings()
{
    QJsonObject o;
    o["onlineWindowSec"] = m_onlineWindowSec;
    o["maxMsgsPerChannel"] = m_maxMsgsPerChannel;
    o["distanceUnit"] = m_distanceUnit;
    emit eventResponse("settingsChanged",
        QVariantList() << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void MeshtasticGatewayPlugin::requestSettings()
{
    emitSettings();
}

void MeshtasticGatewayPlugin::setSetting(const QString& key, const QString& value)
{
    // validate/apply known keys
    if (key == "onlineWindowSec")        m_onlineWindowSec   = qMax(60, value.toInt());
    else if (key == "maxMsgsPerChannel") m_maxMsgsPerChannel = qMax(0, value.toInt());
    else if (key == "distanceUnit")      m_distanceUnit      = (value == "mi") ? "mi" : "km";
    else return;                         // ignore unknown keys

    if (m_db.isOpen()) {
        QSqlQuery q(m_db);
        q.prepare("INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)");
        q.addBindValue(key);
        q.addBindValue(value);
        q.exec();
    }
    emitSettings();          // confirm the new value to the UI
    emitNodes();             // online window / distance may have changed → refresh the node list
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
            saveRelayPref(topic, enabled);

            // Subscribe/unsubscribe the channel's LM topic so we start/stop receiving from it.
            // MUST be deferred: invokeRemoteMethodAsync is misnamed — on a cold token/replica it runs
            // a SYNCHRONOUS capability requestModule + requestObject(waitForSource), up to ~20 s. Doing
            // that inline would block setRelay's return, and since the UI calls us via a SYNCHRONOUS
            // callModule, the whole UI freezes. singleShot(0) lets setRelay return instantly first.
            if (m_deliveryReady && m_delivery) {
                LogosAPIClient* d = m_delivery;
                const bool en = enabled;
                QTimer::singleShot(0, this, [d, topic, en]() {
                    d->invokeRemoteMethodAsync("delivery_module",
                        en ? "subscribe" : "unsubscribe", QVariant(topic), [](QVariant) {});
                });
            }
            emitChannels();
            return;
        }
    }
}

QString MeshtasticGatewayPlugin::status()
{
    QJsonObject o;
    o["nodePresent"] = m_nodePresent;
    o["linkState"] = m_linkState;            // searching | connecting | connected (LoRa node)
    o["deliveryState"] = m_deliveryState;    // down | connecting | ready (Logos Messaging)
    o["nodeName"] = m_nodeName;
    o["channelCount"] = m_channels.size();
    o["nodesTotal"] = m_nodesTotal;     // mesh nodes known to the local node
    o["nodesOnline"] = m_nodesOnline;   // heard within the activity window
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// --- Event emitters: every event carries its full state so the UI never pulls (no blocking IPC) ---

void MeshtasticGatewayPlugin::emitChannels()
{
    emit eventResponse("channelsChanged", QVariantList() << getChannels());
}

void MeshtasticGatewayPlugin::emitStatus()
{
    emit eventResponse("nodeStatus", QVariantList() << status());
}

void MeshtasticGatewayPlugin::emitMessages(int channelIndex)
{
    emit eventResponse("messagesChanged",
                       QVariantList() << channelIndex << getMessages(channelIndex));
}

// Great-circle distance (m) and initial bearing (deg from north) between two lat/lon points.
static double haversineMeters(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double R = 6371000.0, D2R = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * D2R, dLon = (lon2 - lon1) * D2R;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
        + std::cos(lat1 * D2R) * std::cos(lat2 * D2R) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return R * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
static double bearingDegrees(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double D2R = M_PI / 180.0;
    const double y = std::sin((lon2 - lon1) * D2R) * std::cos(lat2 * D2R);
    const double x = std::cos(lat1 * D2R) * std::sin(lat2 * D2R)
        - std::sin(lat1 * D2R) * std::cos(lat2 * D2R) * std::cos((lon2 - lon1) * D2R);
    double b = std::atan2(y, x) / D2R;
    return b < 0 ? b + 360.0 : b;
}

void MeshtasticGatewayPlugin::emitNodes()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Our own position (if known) anchors distance/bearing for every other node.
    bool selfHasPos = false; double selfLat = 0, selfLon = 0;
    if (m_nodes.contains(m_myNum)) {
        const QJsonObject me = m_nodes.value(m_myNum);
        if (me.value("hasPos").toBool()) {
            selfHasPos = true; selfLat = me.value("lat").toDouble(); selfLon = me.value("lon").toDouble();
        }
    }
    QJsonArray arr;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        QJsonObject o = it.value();
        const qint64 lh = qint64(o.value("lastHeard").toDouble());
        o["online"] = lh > 0 && (now - lh) < m_onlineWindowSec;
        o["lastHeardAgo"] = lh > 0 ? double(now - lh) : -1;   // seconds ago; -1 = never/unknown
        if (selfHasPos && o.value("hasPos").toBool() && !o.value("isSelf").toBool()) {
            o["distanceM"] = haversineMeters(selfLat, selfLon, o.value("lat").toDouble(), o.value("lon").toDouble());
            o["bearingDeg"] = bearingDegrees(selfLat, selfLon, o.value("lat").toDouble(), o.value("lon").toDouble());
        }
        arr.append(o);
    }
    emit eventResponse("nodesChanged",
        QVariantList() << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

// Fire-and-forget pull replacements: the UI calls these (return ignored) and renders on the event.
void MeshtasticGatewayPlugin::requestSnapshot()
{
    emitStatus();
    emitChannels();
    emitNodes();
    emitSettings();
}

void MeshtasticGatewayPlugin::requestMessages(int channelIndex)
{
    emitMessages(channelIndex);
}

void MeshtasticGatewayPlugin::requestNodes()
{
    emitNodes();
}

// Live NodeInfo can arrive in bursts (250-node mesh); coalesce into one full-list emit per window
// so the UI doesn't re-parse/re-render the whole node list dozens of times a second (that froze it).
void MeshtasticGatewayPlugin::scheduleNodesEmit()
{
    if (m_nodesEmitPending) return;
    m_nodesEmitPending = true;
    QTimer::singleShot(800, this, [this]() { m_nodesEmitPending = false; emitNodes(); });
}

// TODO: real NodeDB read — the Meshtastic node streams a NodeInfo list (num, user.longName/shortName,
// lastHeard, snr, battery, position). "online" = lastHeard within an activity window (~2h default).
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

// Update the Logos Messaging indicator; only re-emit status when it actually changes (initDelivery
// retries every 500 ms, so unconditional emits would spam the UI).
void MeshtasticGatewayPlugin::setDeliveryState(const QString& s)
{
    if (m_deliveryState == s) return;
    m_deliveryState = s;
    emitStatus();
}

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
            setDeliveryState("down");
        }
        return;
    }

    // Subscribe to inbound LM messages first (event payload: [hash, contentTopic, payloadB64, ts]).
    m_delivery->onEvent(m_deliveryObj, "messageReceived",
        [this](const QString&, const QVariantList& data) { onDeliveryMessage(data); });

    // Edge node on the logos.dev cluster, then subscribe the relaying channels' topics.
    setDeliveryState("connecting");
    const QString cfg = QStringLiteral("{\"mode\":\"Edge\",\"preset\":\"logos.dev\"}");
    qInfo() << "meshtastic_gateway: delivery createNode…";
    m_delivery->invokeRemoteMethodAsync("delivery_module", "createNode", QVariant(cfg),
        [this](QVariant) {
            qInfo() << "meshtastic_gateway: delivery start…";
            m_delivery->invokeRemoteMethodAsync("delivery_module", "start", QVariantList(),
                [this](QVariant) {
                    m_deliveryReady = true;
                    setDeliveryState("ready");
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
        LogosAPIClient* d = m_delivery;
        QTimer::singleShot(0, this, [d, topic]() {
            d->invokeRemoteMethodAsync("delivery_module", "subscribe",
                                       QVariant(topic), [](QVariant) {});
        });
    }
}

void MeshtasticGatewayPlugin::publishToLM(const QString& topic, const QString& text)
{
    if (!m_deliveryReady || !m_delivery) {
        qWarning() << "meshtastic_gateway: delivery not ready — dropping publish to" << topic;
        return;
    }
    // delivery_module.send(contentTopic, payload) base64-encodes payload itself -> pass raw text.
    // Deferred (singleShot 0): publishToLM runs inside sendMessage(), a UI-synchronous callModule —
    // invokeRemoteMethodAsync can block ~20 s on a cold token/replica and would freeze the UI.
    LogosAPIClient* d = m_delivery;
    QTimer::singleShot(0, this, [d, topic, text]() {
        d->invokeRemoteMethodAsync("delivery_module", "send",
                                   QVariant(topic), QVariant(text), [](QVariant) {});
    });
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
    persistMessage(ch, o);
    emitMessages(ch);

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
        m_linkState = "searching";
        emitStatus();
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
                m_linkState = "searching";
                emitStatus();
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
    m_configComplete = false;
    m_wantConfigTries = 0;
    m_linkState = "connecting";   // serial up, waiting for the config burst (UI shows blinking dot)
    emitStatus();
    sendWantConfig();
}

// Ask the radio for its config burst (MyNodeInfo + NodeInfos + Channels + config, then
// config_complete_id echoing our nonce). Opening the USB-CDC port can reset an ESP32-S3, so the
// first request after open is frequently lost while the device re-enumerates — we resend (bounded)
// until config_complete arrives, otherwise the gateway would sit with no channels forever.
void MeshtasticGatewayPlugin::sendWantConfig()
{
    if (!m_serial || !m_serial->isOpen() || m_configComplete) return;

    // Each want_config restarts the burst, so reset the accumulators on every (re)send.
    m_rxBuf.clear();
    m_cfgChannels = QJsonArray();
    m_cfgNodesTotal = m_cfgNodesOnline = 0;

    // Wake a possibly-just-reset device: a run of START2 bytes forces the firmware's frame parser
    // to resync (mirrors the reference Meshtastic client) before we send the request.
    m_serial->write(QByteArray(32, char(MT_START2)));

    m_wantConfigId = 0x6d657368u;   // "mesh"
    meshtastic::ToRadio toRadio;
    toRadio.set_want_config_id(m_wantConfigId);
    m_serial->write(frameToRadio(toRadio.SerializeAsString()));

    if (++m_wantConfigTries <= 5) {
        QTimer::singleShot(3000, this, [this]() {
            if (!m_configComplete) {
                qInfo() << "meshtastic_gateway: no config yet — resending want_config (try"
                        << m_wantConfigTries << ")";
                sendWantConfig();
            }
        });
    } else {
        qWarning() << "meshtastic_gateway: radio not responding to want_config after retries";
    }
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
        if (n.last_heard() && now - qint64(n.last_heard()) < m_onlineWindowSec) m_cfgNodesOnline++;

        QJsonObject o;
        o["num"] = double(n.num());
        o["isSelf"] = (n.num() == m_myNum);
        if (n.has_user()) {
            const QString shortName = QString::fromStdString(n.user().short_name());
            const QString longName = QString::fromStdString(n.user().long_name());
            m_nodeNames[n.num()] = shortName.isEmpty() ? longName : shortName;
            if (n.num() == m_myNum) m_pendingNodeName = longName.isEmpty() ? shortName : longName;
            o["name"] = shortName.isEmpty() ? longName : shortName;
            o["longName"] = longName;
            o["hwModel"] = QString::fromStdString(meshtastic::HardwareModel_Name(n.user().hw_model()));
            o["role"] = QString::fromStdString(meshtastic::Config_DeviceConfig_Role_Name(n.user().role()));
        } else {
            o["name"] = QStringLiteral("!%1").arg(n.num(), 8, 16, QChar('0'));
        }
        o["snr"] = n.snr();
        o["lastHeard"] = double(n.last_heard());
        if (n.has_hops_away()) o["hopsAway"] = int(n.hops_away());
        o["viaMqtt"] = n.via_mqtt();
        o["isFavorite"] = n.is_favorite();
        if (n.has_device_metrics()) {
            const meshtastic::DeviceMetrics& m = n.device_metrics();
            if (m.has_battery_level()) o["battery"] = int(m.battery_level());
            if (m.has_voltage()) o["voltage"] = m.voltage();
            if (m.has_channel_utilization()) o["chUtil"] = m.channel_utilization();
            if (m.has_air_util_tx()) o["airUtil"] = m.air_util_tx();
        }
        if (n.has_position() && n.position().has_latitude_i() && n.position().has_longitude_i()) {
            o["hasPos"] = true;
            o["lat"] = n.position().latitude_i() / 1e7;
            o["lon"] = n.position().longitude_i() / 1e7;
            if (n.position().has_altitude()) o["alt"] = n.position().altitude();
        } else {
            o["hasPos"] = false;
        }
        m_nodes[n.num()] = o;
        if (m_configComplete) scheduleNodesEmit();   // live discovery -> coalesced refresh (avoid flood)
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
    case meshtastic::FromRadio::kConfig:
        // capture the LoRa modem preset so a blank primary channel shows its real name (e.g. MediumFast)
        if (fr.config().has_lora()) {
            m_modemPreset = QString::fromStdString(
                meshtastic::Config_LoRaConfig_ModemPreset_Name(fr.config().lora().modem_preset()));
            qInfo() << "meshtastic_gateway: modem preset" << m_modemPreset;
        }
        break;
    default:
        break;
    }
}

void MeshtasticGatewayPlugin::finalizeConfig()
{
    m_configComplete = true;        // stops the want_config resend loop
    m_nodePresent = true;
    m_linkState = "connected";
    if (!m_pendingNodeName.isEmpty()) m_nodeName = m_pendingNodeName;
    m_nodesTotal = m_cfgNodesTotal;
    m_nodesOnline = m_cfgNodesOnline;
    qInfo() << "meshtastic_gateway: radio config complete —" << m_cfgChannels.size()
            << "channels," << m_nodesTotal << "nodes (" << m_nodesOnline << "online)";
    rebuildChannelsFromMesh(m_cfgChannels);     // emits channelsChanged + (re)subscribes LM topics
    emitStatus();
    emitNodes();
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
        c["displayName"] = !name.isEmpty() ? name
            : (role == "PRIMARY" ? (m_modemPreset.isEmpty() ? QStringLiteral("Primary")
                                                            : prettyPreset(m_modemPreset))
                                 : QStringLiteral("ch%1").arg(idx));
        c["role"] = role;
        c["psk"] = pskB64;
        c["pskStatus"] = pskStatus(pskB64);
        const QString topic = deriveTopic(name, psk, idx);   // real name+psk -> real LM topic
        c["topic"] = topic;
        c["shareUrl"] = channelShareUrl(name, psk);          // meshtastic.org/e/# add-channel URL
        // Relay state: keep an in-session toggle if we have one, else restore the persisted pref.
        c["relaying"] = wasRelaying.contains(idx) ? wasRelaying.value(idx)
                                                  : loadRelayPref(topic, false);
        c["unread"] = hadUnread.value(idx, 0);
        rebuilt.append(c);

        // Restore persisted chat history for this channel (only if we don't already have it in memory,
        // so live messages received this session aren't clobbered on a reconnect/refresh).
        if (!m_messages.contains(idx) || m_messages.value(idx).isEmpty()) {
            const QJsonArray hist = loadMessages(topic, kMsgLoadLimit);
            if (!hist.isEmpty()) {
                m_messages[idx] = hist;
                emitMessages(idx);
            }
        }
    }
    m_channels = rebuilt;
    emitChannels();
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
    persistMessage(channelIndex, o);
    emitMessages(channelIndex);

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

// Send an AdminMessage to our own node (local admin over USB — no LoRa hop, just a serial write).
static void sendAdminMsg(QSerialPort* serial, quint32 myNum, const meshtastic::AdminMessage& admin)
{
    if (!serial || !serial->isOpen()) {
        qWarning() << "meshtastic_gateway: admin TX dropped — serial not open"; return;
    }
    meshtastic::ToRadio toRadio;
    meshtastic::MeshPacket* p = toRadio.mutable_packet();
    p->set_to(myNum ? myNum : MT_BROADCAST);
    meshtastic::Data* d = p->mutable_decoded();
    d->set_portnum(meshtastic::ADMIN_APP);
    const std::string payload = admin.SerializeAsString();
    d->set_payload(payload.data(), payload.size());
    serial->write(frameToRadio(toRadio.SerializeAsString()));
}

// Write the node's owner name to the radio: AdminMessage{set_owner:User}.
void MeshtasticGatewayPlugin::setOwner(const QString& longName, const QString& shortName)
{
    const QString ln = longName.trimmed();
    const QString sn = shortName.trimmed();
    if (ln.isEmpty()) return;

    meshtastic::AdminMessage admin;
    meshtastic::User* u = admin.mutable_set_owner();
    if (m_myNum) u->set_id(QStringLiteral("!%1").arg(m_myNum, 8, 16, QChar('0')).toStdString());
    u->set_long_name(ln.toStdString());
    u->set_short_name(sn.toStdString());
    sendAdminMsg(m_serial, m_myNum, admin);
    qInfo() << "meshtastic_gateway: setOwner ->" << ln << "/" << sn;

    // Optimistic UI update; the radio also re-broadcasts NodeInfo with the new name shortly.
    m_nodeName = ln;
    if (m_myNum && m_nodes.contains(m_myNum)) {
        QJsonObject o = m_nodes.value(m_myNum);
        o["name"] = sn.isEmpty() ? ln : sn;
        o["longName"] = ln;
        m_nodes[m_myNum] = o;
        m_nodeNames[m_myNum] = sn.isEmpty() ? ln : sn;
    }
    emitStatus();
    emitNodes();
}

// Add a SECONDARY channel: AdminMessage{set_channel:Channel} at the lowest free slot (1..7).
void MeshtasticGatewayPlugin::createChannel(const QString& name, const QString& pskB64)
{
    const QString nm = name.trimmed();
    if (nm.isEmpty()) return;

    QSet<int> used;
    for (const auto& v : m_channels) used.insert(v.toObject()["channelIndex"].toInt());
    int idx = -1;
    for (int i = 1; i <= 7; ++i) if (!used.contains(i)) { idx = i; break; }
    if (idx < 0) { qWarning() << "meshtastic_gateway: no free channel slot (1-7 full)"; return; }

    QByteArray psk = QByteArray::fromBase64(pskB64.toUtf8());
    if (psk.isEmpty()) {                       // generate a fresh 16-byte key for a private channel
        psk.resize(16);
        for (int i = 0; i < 16; ++i) psk[i] = char(QRandomGenerator::system()->bounded(256));
    }

    meshtastic::AdminMessage admin;
    meshtastic::Channel* ch = admin.mutable_set_channel();
    ch->set_index(idx);
    ch->set_role(meshtastic::Channel_Role_SECONDARY);
    meshtastic::ChannelSettings* st = ch->mutable_settings();
    st->set_name(nm.toStdString());
    st->set_psk(psk.constData(), psk.size());
    sendAdminMsg(m_serial, m_myNum, admin);
    qInfo() << "meshtastic_gateway: createChannel" << idx << nm;

    // optimistic add
    const QString b64 = QString::fromLatin1(psk.toBase64());
    QJsonObject c;
    c["channelIndex"] = idx;
    c["name"] = nm;
    c["displayName"] = nm;
    c["role"] = "SECONDARY";
    c["psk"] = b64;
    c["pskStatus"] = pskStatus(b64);
    c["topic"] = deriveTopic(nm, psk, idx);
    c["shareUrl"] = channelShareUrl(nm, psk);
    c["relaying"] = false;
    c["unread"] = 0;
    m_channels.append(c);
    emitChannels();
}

// Remove a channel: AdminMessage{set_channel:Channel{role=DISABLED}}. Never the primary (index 0).
void MeshtasticGatewayPlugin::deleteChannel(int index)
{
    if (index <= 0) { qWarning() << "meshtastic_gateway: refusing to delete channel" << index; return; }

    meshtastic::AdminMessage admin;
    meshtastic::Channel* ch = admin.mutable_set_channel();
    ch->set_index(index);
    ch->set_role(meshtastic::Channel_Role_DISABLED);
    sendAdminMsg(m_serial, m_myNum, admin);
    qInfo() << "meshtastic_gateway: deleteChannel" << index;

    // optimistic remove + unsubscribe its LM topic if it was relaying
    QJsonArray rebuilt;
    for (const auto& v : m_channels) {
        const QJsonObject c = v.toObject();
        if (c["channelIndex"].toInt() == index) {
            if (c["relaying"].toBool() && m_deliveryReady && m_delivery) {
                LogosAPIClient* dlv = m_delivery;
                const QString topic = c["topic"].toString();
                QTimer::singleShot(0, this, [dlv, topic]() {
                    dlv->invokeRemoteMethodAsync("delivery_module", "unsubscribe", QVariant(topic), [](QVariant) {});
                });
            }
            continue;   // drop it
        }
        rebuilt.append(c);
    }
    m_channels = rebuilt;
    m_messages.remove(index);
    if (m_db.isOpen()) { /* keep history in DB; only the live channel is removed */ }
    emitChannels();
}

// --- Chat ---------------------------------------------------------------

int MeshtasticGatewayPlugin::monotonicTs()
{
    // Monotonic ordering counter, persisted across restarts (resumed in openDb). Used only for
    // message ordering, not as a wall clock.
    return ++m_clock;
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
    persistMessage(channelIndex, o);
    // ALWAYS transmit on LoRa — a typed message is a chat message (DATAFLOWS.md Flow C). maybeRelay()
    // above recorded the fingerprint + did the LM publish; this puts it on the mesh for other nodes.
    sendToMesh(channelIndex, body);

    emitMessages(channelIndex);

    // Stub: simulate the mesh ack landing ~1.4s later so the status icon visibly cycles
    // enroute -> delivered. Real impl flips this on the node's ACK packet for this packet id.
    QTimer::singleShot(1400, this, [this, channelIndex, id]() {
        QJsonArray& arr = m_messages[channelIndex];
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject m = arr[i].toObject();
            if (m["id"].toInt() == id) {
                m["ackStatus"] = QStringLiteral("delivered");
                arr[i] = m;
                persistMessage(channelIndex, m);
                emitMessages(channelIndex);
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
            emitChannels();
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
        persistMessage(channelIndex, m);
        emitMessages(channelIndex);
        return;
    }
}
