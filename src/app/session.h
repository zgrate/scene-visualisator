#pragma once
// App state shared by QML and the viewport: document, fixture library, DMX engine, selection. Lives on the GUI
// thread; the render thread reads it only from ViewportItem's synchronize(), while the GUI thread is blocked.
#include "core/document.h"
#include "protocol/dmx_engine.h"
#include "render/scene_builder.h"

#include <QImage>
#include <QObject>
#include <memory>
#include <optional>
#include <qqmlintegration.h>
#include <set>

class QJSEngine;
class QQmlEngine;

class Session : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    explicit Session(QObject *parent = nullptr);
    ~Session() override;
    static Session *instance(); // the one created in main()
    static Session *create(QQmlEngine *, QJSEngine *);

    sv::Document &document() { return document_; }
    // Immutable copy of the document for the render thread, replaced after every change.
    std::shared_ptr<const sv::json> snapshot() const { return snapshot_; }
    const sv::render::FixtureLibrary &library() const { return library_; }
    sv::proto::DmxEngine &dmx() { return dmx_; }
    const QImage &composition() const { return composition_; }
    const std::set<std::string> &selection() const { return selection_; }
    quint64 revision() const { return revision_; } // bumps on any document or selection change

    void set_project(sv::json project);
    void select(const std::optional<std::string> &uuid, bool add);
    void start_dmx(); // logs and carries on if a port is taken
    void set_generator(sv::proto::Pattern pattern);

  Q_SIGNALS:
    void changed();

  private:
    void refresh();

    sv::Document document_;
    std::shared_ptr<const sv::json> snapshot_;
    sv::render::FixtureLibrary library_;
    sv::proto::DmxEngine dmx_;
    sv::proto::Pattern pattern_ = sv::proto::Pattern::off;
    QImage composition_;
    std::set<std::string> selection_;
    quint64 revision_ = 0;
};
