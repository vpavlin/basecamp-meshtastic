#include "meshcore_radio.h"

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QDebug>

using meshcore::Bytes;

static QByteArray toQB(const Bytes& b) { return QByteArray(reinterpret_cast<const char*>(b.data()), int(b.size())); }
static QString b64(const Bytes& b) { return QString::fromLatin1(toQB(b).toBase64()); }

MeshCoreRadio::MeshCoreRadio(QObject* parent) : MeshRadio(parent) {}
MeshCoreRadio::~MeshCoreRadio() { if (m_serial && m_serial->isOpen()) m_serial->close(); }

void MeshCoreRadio::start() { openSerial(); }

void MeshCoreRadio::stop()
{
    if (m_pollTimer) m_pollTimer->stop();
    if (m_serial && m_serial->isOpen()) m_serial->close();
}

void MeshCoreRadio::openSerial()
{
    if (m_serial && m_serial->isOpen()) return;

    QString port = qEnvironmentVariable("MESHCORE_DEV");
    if (port.isEmpty()) port = qEnvironmentVariable("MESHTASTIC_DEV");   // same USB port autodetect as the other backend
    if (port.isEmpty()) {
        for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
            const quint16 vid = info.vendorIdentifier();
            const QString d = (info.manufacturer() + " " + info.description()).toLower();
            if (vid == 0x303a || vid == 0x10c4 || vid == 0x1a86 || d.contains("heltec") || d.contains("meshcore")) {
                port = info.systemLocation();
                break;
            }
        }
    }
    if (port.isEmpty()) {
        if (m_serialTries++ % 6 == 0) qInfo() << "meshcore_radio: no serial device found — waiting";
        emit linkStateChanged(QStringLiteral("searching"), false, QString());
        QTimer::singleShot(5000, this, [this]() { openSerial(); });
        return;
    }

    if (!m_serial) {
        m_serial = new QSerialPort(this);
        connect(m_serial, &QSerialPort::readyRead, this, [this]() { onReadyRead(); });
        connect(m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError) {
                qWarning() << "meshcore_radio: serial error" << e << "— reconnecting";
                if (m_serial->isOpen()) m_serial->close();
                m_handshaked = m_discovered = false;
                emit linkStateChanged(e == QSerialPort::PermissionError ? QStringLiteral("noperm")
                                                                        : QStringLiteral("searching"),
                                      false, QString());
                QTimer::singleShot(3000, this, [this]() { openSerial(); });
            }
        });
    }
    m_serial->setPortName(port);
    m_serial->setBaudRate(QSerialPort::Baud115200);
    if (!m_serial->open(QIODevice::ReadWrite)) {
        qWarning() << "meshcore_radio: cannot open" << port << "—" << m_serial->errorString();
        emit linkStateChanged(m_serial->error() == QSerialPort::PermissionError ? QStringLiteral("noperm")
                                                                                : QStringLiteral("searching"),
                              false, QString());
        QTimer::singleShot(3000, this, [this]() { openSerial(); });
        return;
    }

    qInfo() << "meshcore_radio: serial open on" << port << "— APP_START handshake";
    m_handshaked = m_discovered = false;
    m_reader = meshcore::FrameReader();
    emit linkStateChanged(QStringLiteral("connecting"), false, QString());
    sendFrame(meshcore::cmdAppStart("logos-gw"));

    // Safety-net poll: most inbound text arrives via the MESSAGES_WAITING push, but poll periodically
    // in case a push is missed, and re-send APP_START until the handshake completes.
    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout, this, [this]() {
            if (!m_serial || !m_serial->isOpen()) return;
            if (!m_handshaked) sendFrame(meshcore::cmdAppStart("logos-gw"));
            else               drainNext();    // pull any queued message
        });
    }
    m_pollTimer->start(4000);
}

void MeshCoreRadio::sendFrame(const Bytes& inner)
{
    if (!m_serial || !m_serial->isOpen()) return;
    m_serial->write(toQB(meshcore::wrapTx(inner)));
}

void MeshCoreRadio::onReadyRead()
{
    const QByteArray d = m_serial->readAll();
    m_reader.feed(reinterpret_cast<const uint8_t*>(d.constData()), size_t(d.size()));
    while (auto f = m_reader.next()) handleFrame(*f);
}

void MeshCoreRadio::handleFrame(const Bytes& f)
{
    if (f.empty()) return;
    switch (f[0]) {
    case meshcore::RESP_SELF_INFO:
        if (auto si = meshcore::parseSelfInfo(f)) {
            m_pubKey   = toQB(si->publicKey);
            m_nodeName = QString::fromStdString(si->name);
            qInfo() << "meshcore_radio: SELF_INFO name" << m_nodeName << "sf" << si->sf << "cr" << si->cr;
            m_handshaked = true;
            emit linkStateChanged(QStringLiteral("connecting"), true, m_nodeName);
            m_chan.clear();
            requestChannel(0);             // begin channel discovery
        }
        break;

    case meshcore::RESP_CHANNEL_INFO:
        if (auto ci = meshcore::parseChannelInfo(f)) {
            if (!ci->isEmpty) m_chan.insert(ci->index, *ci);
            if (m_scanIdx >= 0 && m_scanIdx + 1 < m_maxChannels) requestChannel(m_scanIdx + 1);
            else finishDiscovery();
        }
        break;

    case meshcore::RESP_CHANNEL_MSG:
    case meshcore::RESP_CHANNEL_MSG_V3:
        if (auto m = meshcore::parseTextMessage(f); m && m->isChannel) {
            emit meshMessage(m->channelIndex, QString(), QString::fromStdString(m->text));
            drainNext();                   // pull the next queued message, if any
        }
        break;

    case meshcore::RESP_CONTACT_MSG:
    case meshcore::RESP_CONTACT_MSG_V3:
        // Direct (contact) messages aren't channel traffic — log + drain, but don't relay.
        qInfo() << "meshcore_radio: contact (DM) message — not relayed";
        drainNext();
        break;

    case meshcore::PUSH_MSGS_WAITING:
        drainNext();
        break;

    case meshcore::RESP_NO_MORE_MSGS:      // 0x0A: end of the GET_MESSAGE drain
        break;
    case meshcore::RESP_MSG_SENT:
        break;
    case meshcore::RESP_OK:
    case meshcore::RESP_ERROR:
        if (f[0] == meshcore::RESP_ERROR) qWarning() << "meshcore_radio: radio returned ERROR";
        break;
    default:
        break;
    }
}

void MeshCoreRadio::requestChannel(int idx)
{
    m_scanIdx = idx;
    sendFrame(meshcore::cmdGetChannelInfo(uint8_t(idx)));
}

void MeshCoreRadio::finishDiscovery()
{
    m_scanIdx = -1;
    QJsonArray chans;
    for (auto it = m_chan.constBegin(); it != m_chan.constEnd(); ++it) {
        const meshcore::ChannelInfo& c = it.value();
        QJsonObject o;
        o["channelIndex"] = c.index;
        o["name"]         = QString::fromStdString(c.name);
        o["psk"]          = b64(c.secret);                              // 16-byte secret -> base64 (used as the channel key)
        o["role"]         = c.index == 0 ? "PRIMARY" : "SECONDARY";     // ch0 (Public) is the primary
        chans.append(o);
    }
    qInfo() << "meshcore_radio: discovery complete —" << chans.size() << "channels";
    emit channelsDiscovered(chans);
    emit linkStateChanged(QStringLiteral("connected"), true, m_nodeName);

    // Minimal NodeDB: just self (MeshCore contacts could populate this later).
    QJsonArray nodes;
    QJsonObject self;
    self["num"] = 0; self["isSelf"] = true; self["name"] = m_nodeName;
    nodes.append(self);
    emit nodesDiscovered(nodes, 1, 1);

    m_discovered = true;
    drainNext();    // drain anything queued while we were connecting
}

void MeshCoreRadio::drainNext()
{
    if (m_handshaked) sendFrame(meshcore::cmdGetMessage());
}

void MeshCoreRadio::sendToMesh(int channelIndex, const QString& text)
{
    if (!m_serial || !m_serial->isOpen()) {
        qWarning() << "meshcore_radio: serial not open — TX dropped on ch" << channelIndex;
        return;
    }
    const quint32 ts = quint32(QDateTime::currentSecsSinceEpoch());
    sendFrame(meshcore::cmdSendChannelMessage(uint8_t(channelIndex), ts, text.toStdString()));
}

void MeshCoreRadio::requestNodes()
{
    QJsonArray nodes;
    QJsonObject self; self["num"] = 0; self["isSelf"] = true; self["name"] = m_nodeName;
    nodes.append(self);
    emit nodesDiscovered(nodes, 1, 1);
}

void MeshCoreRadio::createChannel(const QString& name, const QByteArray& key)
{
    const QString nm = name.trimmed();
    if (nm.isEmpty()) return;

    int idx = -1;
    for (int i = 1; i < m_maxChannels; ++i) if (!m_chan.contains(i)) { idx = i; break; }
    if (idx < 0) { qWarning() << "meshcore_radio: no free channel slot"; return; }

    QByteArray secret = key;
    if (secret.size() != 16) {                          // generate a fresh 16-byte secret if none/short
        secret.resize(16);
        for (int i = 0; i < 16; ++i) secret[i] = char(QRandomGenerator::system()->bounded(256));
    }
    Bytes sec(secret.begin(), secret.end());
    sendFrame(meshcore::cmdSetChannel(uint8_t(idx), nm.toStdString(), sec));
    qInfo() << "meshcore_radio: createChannel" << idx << nm;
    // Re-scan after the radio has applied the SET so channelsDiscovered reflects the new channel.
    QTimer::singleShot(400, this, [this]() { m_chan.clear(); requestChannel(0); });
}

void MeshCoreRadio::setOwner(const QString& longName, const QString& /*shortName*/)
{
    // MeshCore device-name set isn't wired yet; configure via the MeshCore app for now.
    qInfo() << "meshcore_radio: setOwner not supported (ignored) —" << longName;
}

void MeshCoreRadio::addChannelFromUrl(const QString& url)
{
    qInfo() << "meshcore_radio: addChannelFromUrl not supported (ignored) —" << url;
}

void MeshCoreRadio::deleteChannel(int channelIndex)
{
    if (channelIndex <= 0) { qWarning() << "meshcore_radio: refusing to delete channel" << channelIndex; return; }
    sendFrame(meshcore::cmdSetChannel(uint8_t(channelIndex), std::string(), Bytes(16, 0)));   // clear the slot
    qInfo() << "meshcore_radio: deleteChannel" << channelIndex;
    QTimer::singleShot(400, this, [this]() { m_chan.clear(); requestChannel(0); });
}
