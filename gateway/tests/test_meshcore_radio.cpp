// Live hardware harness for MeshCoreRadio — drives a real MeshCore companion radio over USB serial
// and prints the MeshRadio signals (link state, discovered channels, inbound mesh messages). No
// logoscore / delivery_module — just the backend against the radio.
//   MESHCORE_DEV=/dev/ttyACM0 ./mcradiotest [--send "<text>" --chan N]
#include "meshcore_radio.h"
#include <QCoreApplication>
#include <QTimer>
#include <QJsonDocument>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QString sendText; int sendChan = -1;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--send" && i + 1 < args.size()) sendText = args[++i];
        else if (args[i] == "--chan" && i + 1 < args.size()) sendChan = args[++i].toInt();
    }

    MeshCoreRadio radio;
    QObject::connect(&radio, &MeshRadio::linkStateChanged, [](const QString& s, bool p, const QString& n) {
        qInfo().noquote() << "LINK:" << s << "present=" << p << "name=" << n;
    });
    QObject::connect(&radio, &MeshRadio::channelsDiscovered, [&](const QJsonArray& ch) {
        qInfo().noquote() << "CHANNELS:" << QJsonDocument(ch).toJson(QJsonDocument::Compact);
        if (!sendText.isEmpty() && sendChan >= 0) {
            QTimer::singleShot(800, [&]() {
                qInfo().noquote() << "SENDING on ch" << sendChan << ":" << sendText;
                radio.sendToMesh(sendChan, sendText);
            });
        }
    });
    QObject::connect(&radio, &MeshRadio::meshMessage, [](int c, const QString& f, const QString& t) {
        qInfo().noquote() << "MESH-MSG: ch" << c << "from" << f << "text:" << t;
    });
    QObject::connect(&radio, &MeshRadio::nodesDiscovered, [](const QJsonArray&, int tot, int on) {
        qInfo().noquote() << "NODES: total=" << tot << "online=" << on;
    });

    radio.start();
    QTimer::singleShot(14000, &app, &QCoreApplication::quit);
    return app.exec();
}
