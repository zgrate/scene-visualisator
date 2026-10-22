#include "session.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[]) {
    // Same render loop on every OS, so Linux dev builds exercise the render-thread code paths.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP"))
        qputenv("QSG_RENDER_LOOP", "threaded");

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("Scene Visualiser");
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QQuickStyle::setStyle("Fusion");

    QCommandLineParser args;
    args.addHelpOption();
    args.addVersionOption();
    QCommandLineOption shot("screenshot", "Save the window to <png> after the first frames, then exit.", "png");
    args.addOption(shot);
    QCommandLineOption generator("generator", "Run an internal test pattern: off, full, chase, rainbow, sweep.",
                                 "pattern", "off");
    args.addOption(generator);
    args.process(app);

    Session session;
    // shortcut: always opens the demo rig until File > Open lands (M5).
    session.set_project(sv::demo_project());
    session.start_dmx();
    static const std::map<QString, sv::proto::Pattern> patterns{{"off", sv::proto::Pattern::off},
                                                                {"full", sv::proto::Pattern::full},
                                                                {"chase", sv::proto::Pattern::chase},
                                                                {"rainbow", sv::proto::Pattern::rainbow},
                                                                {"sweep", sv::proto::Pattern::sweep}};
    if (auto it = patterns.find(args.value(generator)); it != patterns.end())
        session.set_generator(it->second);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("SceneVisualiser", "Main");

    if (args.isSet(shot) && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString path = args.value(shot);
        // After the first real frame; an idle scene may never swap again, and grabWindow renders anew.
        QObject::connect(
            window, &QQuickWindow::frameSwapped, &app,
            [=] { QTimer::singleShot(0, [=] { QCoreApplication::exit(window->grabWindow().save(path) ? 0 : 2); }); },
            Qt::SingleShotConnection);
    }
    return app.exec();
}
