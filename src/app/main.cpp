#include "session.h"

#include "render/test_pattern.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMutex>
#include <QQmlApplicationEngine>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

namespace {
// Every Qt message also goes to AppDataLocation/logs/scene-visualiser.log, rotated at launch with three kept.
// Plain Qt instead of spdlog (plan M6): one handler and a file, not worth a dependency.
QFile log_file;
QMutex log_mutex;
QtMessageHandler default_handler = nullptr;

// image://composition/<anything> is the current composition (or live frame) for the Mapping screen; QML changes the
// id to reload.
class CompositionProvider : public QQuickImageProvider {
  public:
    CompositionProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *size, const QSize &) override {
        quint64 serial = 0; // 0: always the current frame of a live source
        const Session *s = Session::instance();
        QImage img = s->media().latest(serial);
        if (img.isNull())
            img = s->composition();
        else if (img.size() != s->composition().size())
            img = sv::render::make_composition(*s->snapshot(), img); // placed by source.fit, as on the walls
        *size = img.size();
        return img;
    }
};

void log_message(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    static const char *levels[] = {"debug", "warning", "critical", "fatal", "info"};
    {
        QMutexLocker lock(&log_mutex);
        if (log_file.isOpen()) {
            log_file.write(QStringLiteral("%1 %2 %3\n")
                               .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), levels[type], message)
                               .toUtf8());
            log_file.flush();
        }
    }
    default_handler(type, context, message);
}

void open_log() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    QDir().mkpath(dir);
    const auto name = [&](int i) { return dir + "/scene-visualiser.log" + (i ? "." + QString::number(i) : ""); };
    QFile::remove(name(3));
    for (int i = 2; i >= 0; --i)
        QFile::rename(name(i), name(i + 1));
    log_file.setFileName(name(0));
    if (log_file.open(QIODevice::WriteOnly | QIODevice::Text))
        default_handler = qInstallMessageHandler(log_message);
}
} // namespace

int main(int argc, char *argv[]) {
    // Same render loop on every OS, so Linux dev builds exercise the render-thread code paths.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP"))
        qputenv("QSG_RENDER_LOOP", "threaded");

#ifdef Q_OS_WIN
    // B1: Spout shares D3D11 textures; pin Qt's default so a sender opens straight into our device.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("Scene Visualiser");
    QGuiApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QQuickStyle::setStyle("Fusion");
    open_log();

    QCommandLineParser args;
    args.addHelpOption();
    args.addVersionOption();
    QCommandLineOption shot("screenshot", "Save the window to <png> after the first frames, then exit.", "png");
    args.addOption(shot);
    QCommandLineOption generator("generator", "Run an internal test pattern: off, full, chase, rainbow, sweep.",
                                 "pattern", "off");
    args.addOption(generator);
    QCommandLineOption screen("screen", "Start on a screen: design, patch, dmx, mapping, project.", "name", "design");
    args.addOption(screen);
    args.addPositionalArgument("project", "A .svproj file to open; the demo rig when omitted.");
    args.process(app);

    Session session(nullptr);
    if (args.positionalArguments().isEmpty())
        session.set_project(sv::demo_project(), QStringLiteral("Demo rig"));
    else if (const QString error = session.openProject(QUrl::fromLocalFile(args.positionalArguments().first()));
             !error.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
    }
    session.start_dmx();
    static const std::map<QString, sv::proto::Pattern> patterns{{"off", sv::proto::Pattern::off},
                                                                {"full", sv::proto::Pattern::full},
                                                                {"chase", sv::proto::Pattern::chase},
                                                                {"rainbow", sv::proto::Pattern::rainbow},
                                                                {"sweep", sv::proto::Pattern::sweep}};
    if (auto it = patterns.find(args.value(generator)); it != patterns.end())
        session.set_generator(it->second);

    QQmlApplicationEngine engine;
    engine.addImageProvider("composition", new CompositionProvider);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("SceneVisualiser", "Main");
    if (!engine.rootObjects().isEmpty())
        engine.rootObjects().first()->setProperty("screen", args.value(screen));

    if (args.isSet(shot) && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString path = args.value(shot);
        // After the first real frame; an idle scene may never swap again, and grabWindow renders anew. A live media
        // source gets up to 5 s to deliver its first frame, so the shot shows it.
        QObject::connect(
            window, &QQuickWindow::frameSwapped, &app,
            [=, &session] {
                auto *poll = new QTimer(window);
                QObject::connect(poll, &QTimer::timeout, window, [=, &session, tries = 0]() mutable {
                    if (session.media().active() && !session.media().status().signal && ++tries < 50)
                        return;
                    poll->stop();
                    QTimer::singleShot(session.media().active() ? 600 : 100,
                                       [=] { QCoreApplication::exit(window->grabWindow().save(path) ? 0 : 2); });
                });
                poll->start(100);
            },
            Qt::SingleShotConnection);
    }
    return app.exec();
}
