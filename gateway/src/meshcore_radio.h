#ifndef MESHCORE_RADIO_H
#define MESHCORE_RADIO_H

#include "mesh_radio.h"
#include "meshcore_protocol.h"
#include <QMap>
#include <QVector>
#include <QByteArray>

class QSerialPort;
class QTimer;

// MeshCore backend: speaks the Companion Radio protocol over USB serial (see meshcore_protocol.h).
// Wraps the (unit- and hardware-validated) pure-C++ codec around QSerialPort and presents the
// mesh-agnostic MeshRadio interface the gateway plugin drives. Handshake: APP_START -> SELF_INFO,
// then GET_CHANNEL_INFO 0..N to discover channels; inbound text arrives via the MESSAGES_WAITING
// push -> GET_MESSAGE drain loop; outbound via SEND_CHANNEL_MESSAGE.
class MeshCoreRadio : public MeshRadio
{
    Q_OBJECT
public:
    explicit MeshCoreRadio(QObject* parent = nullptr);
    ~MeshCoreRadio() override;

    QString protocol() const override { return QStringLiteral("meshcore"); }
    void start() override;
    void stop() override;
    void sendToMesh(int channelIndex, const QString& text) override;
    void requestNodes() override;
    void setOwner(const QString& longName, const QString& shortName) override;
    void createChannel(const QString& name, const QByteArray& key) override;
    void addChannelFromUrl(const QString& url) override;
    void deleteChannel(int channelIndex) override;

private:
    void openSerial();
    void onReadyRead();
    void sendFrame(const meshcore::Bytes& inner);   // wrap + write
    void handleFrame(const meshcore::Bytes& f);
    void requestChannel(int idx);                   // GET_CHANNEL_INFO
    void finishDiscovery();                          // emit channelsDiscovered + connected
    void drainNext();                                // GET_MESSAGE (pull one queued message)
    void requestContacts();                          // GET_CONTACTS (refresh the contact list)
    void buildAndEmitNodes();                        // self + contacts -> nodesDiscovered

    QSerialPort* m_serial = nullptr;
    meshcore::FrameReader m_reader;
    QTimer* m_pollTimer = nullptr;

    QString m_nodeName;
    QByteArray m_pubKey;                             // local node public key (32B)
    double m_selfLat = 0, m_selfLon = 0;             // local node position (0 = unknown)
    QMap<int, meshcore::ChannelInfo> m_chan;         // discovered channels by index
    QVector<meshcore::Contact> m_contacts;           // MeshCore contacts (the "nodes"), rebuilt per GET_CONTACTS
    int m_scanIdx = -1;                              // current GET_CHANNEL_INFO index, -1 = not scanning
    int m_maxChannels = 8;
    bool m_handshaked = false;
    bool m_discovered = false;
    int m_serialTries = 0;
};

#endif // MESHCORE_RADIO_H
