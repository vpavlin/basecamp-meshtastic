// logoscore main — patched for headless relay use (basecamp-meshtastic / pi-stack.yml).
//
// Why this differs from upstream app/main.cpp: the stock launcher loads all -l modules at startup,
// immediately after logos_core_start() — i.e. INSIDE the ~1s window where the capability module is
// still establishing the core<->capability auth-token chain (plugin_manager's single-shot 1s
// informModuleToken timer). Loading the relay modules during that race makes informModuleToken
// return false, which leaves delivery unauthorized to PUSH events to the gateway consumer — so
// LM->mesh (onDeliveryMessage) never fires under headless logoscore, even though method calls work.
//
// The Basecamp GUI never hits this: it brings up capability (+ package_manager) first, then loads
// modules ON-DEMAND from the UI (long after capability is ready) via
// logos_core_load_plugin_with_dependencies(). This launcher mimics that: load capability_module +
// package_manager first, then defer the relay modules to a timer (after the event loop starts and
// capability's token chain is set up), using the same dependency-aware loader the UI uses.
//
// Remote mode (default — no logos_core_set_mode call). Local mode is avoided: it fails to load
// capability_module in-process on this stack.

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QStringList>
#include <cstdio>
#include "logos_core.h"
#include "command_line_parser.h"
#include "plugin_manager.h"
#include "call_executor.h"

// Custom message handler that ensures immediate flushing of output (CI timeouts otherwise).
void flushingMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    QByteArray localMsg = msg.toLocal8Bit();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";
    switch (type) {
    case QtDebugMsg:    fprintf(stderr, "Debug: %s\n", localMsg.constData()); break;
    case QtInfoMsg:     fprintf(stderr, "Info: %s\n", localMsg.constData()); break;
    case QtWarningMsg:  fprintf(stderr, "Warning: %s\n", localMsg.constData()); break;
    case QtCriticalMsg: fprintf(stderr, "Critical: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function); break;
    case QtFatalMsg:    fprintf(stderr, "Fatal: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function); fflush(stderr); abort();
    }
    fflush(stderr);
}

int main(int argc, char *argv[]) {
    qInstallMessageHandler(flushingMessageHandler);

    QCoreApplication app(argc, argv);
    app.setApplicationName("logoscore");
    app.setApplicationVersion("1.0");

    CoreArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    logos_core_init(argc, argv);
    // Remote mode (default). Do NOT call logos_core_set_mode(1) — Local mode breaks capability load here.

    if (!args.modulesDir.isEmpty()) {
        logos_core_add_plugins_dir(args.modulesDir.toUtf8().constData());
    }

    logos_core_start();

    // 1) Bootstrap: bring up capability + package_manager FIRST so the core<->capability token
    //    chain is established before any relay module loads.
    const QStringList bootstrap = { QStringLiteral("capability_module"), QStringLiteral("package_manager") };
    for (const QString& m : bootstrap) {
        qDebug() << "[launcher] bootstrap loading:" << m;
        if (!logos_core_load_plugin(m.toUtf8().constData())) {
            qWarning() << "[launcher] bootstrap: failed to load" << m;
        }
    }

    // 2) Everything else from -l is DEFERRED until after capability is ready (GUI-style), loaded with
    //    the dependency-aware loader so deps (e.g. delivery_module) come up in the right order.
    QStringList deferred;
    for (const QString& moduleName : args.loadModules) {
        const QString t = moduleName.trimmed();
        if (!t.isEmpty() && !bootstrap.contains(t)) {
            deferred.append(t);
        }
    }
    if (!deferred.isEmpty()) {
        QTimer::singleShot(5000, [deferred]() {
            qDebug() << "[launcher] deferred (GUI-style) load of:" << deferred;
            for (const QString& m : deferred) {
                if (!logos_core_load_plugin_with_dependencies(m.toUtf8().constData())) {
                    qWarning() << "[launcher] deferred: failed to load" << m;
                }
            }
        });
    }

    if (!args.calls.isEmpty()) {
        int callResult = CallExecutor::executeCalls(args.calls);
        if (args.quitOnFinish || callResult != 0) {
            return callResult;
        }
    }

    return logos_core_exec();
}
