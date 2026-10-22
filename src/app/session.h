#pragma once
// App state shared by QML and the viewport: document, fixture library, DMX engine, selection. Lives on the GUI
// thread; the render thread reads it only from ViewportItem's synchronize(), while the GUI thread is blocked.
#include "core/document.h"
#include "protocol/dmx_engine.h"
#include "render/scene_builder.h"

#include <QImage>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector3D>
#include <filesystem>
#include <map>
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
    Q_PROPERTY(QString projectName READ project_name NOTIFY changed)
    Q_PROPERTY(QVariantList objects READ objects NOTIFY changed)
    Q_PROPERTY(QVariantMap current READ current NOTIFY changed) // first selected object plus derived fields
    Q_PROPERTY(int selectionCount READ selection_count NOTIFY changed)
    Q_PROPERTY(QStringList fixtureTypes READ fixture_types NOTIFY changed)
    Q_PROPERTY(QString filePath READ file_path NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(bool canUndo READ can_undo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ can_redo NOTIFY changed)
    Q_PROPERTY(QVariantMap layout READ layout NOTIFY changed)
    Q_PROPERTY(QVariantList universes READ universes NOTIFY changed) // patched universes, ascending
    Q_PROPERTY(QVariantMap dmxStatus READ dmx_status NOTIFY dmxChanged) // {artnet, sacn: rate or -1, perUniverse}

  public:
    explicit Session(QObject *parent); // no default: QML would construct its own instead of calling create()
    ~Session() override;
    static Session *instance(); // the one created in main()
    static Session *create(QQmlEngine *, QJSEngine *);

    sv::Document &document() { return document_; }
    // Immutable copy of the document for the render thread, replaced after every change.
    std::shared_ptr<const sv::json> snapshot() const { return snapshot_; }
    const sv::render::FixtureLibrary &library() const { return library_; }
    // Immutable copy for the render thread, replaced when a GDTF is added.
    std::shared_ptr<const sv::render::FixtureLibrary> library_snapshot() const { return library_snapshot_; }
    sv::proto::DmxEngine &dmx() { return dmx_; }
    const QImage &composition() const { return composition_; }
    const std::set<std::string> &selection() const { return selection_; }
    quint64 revision() const { return revision_; } // bumps on any document or selection change

    void set_project(sv::json project, QString name = QStringLiteral("Untitled"));
    void select(const std::optional<std::string> &uuid, bool add);
    void start_dmx(); // logs and carries on if a port is taken
    void set_generator(sv::proto::Pattern pattern);
    // Viewport drag: preview moves only the render snapshot; commit writes one undoable step.
    void preview_move(const std::string &uuid, QVector3D position);
    void commit_move();
    const std::vector<sv::proto::UniverseStatus> &status() const { return status_; }

    QString project_name() const { return name_; }
    QString file_path() const { return QString::fromStdString(path_.string()); }
    bool dirty() const { return document_.dirty(); }
    QVariantList objects() const;
    QVariantMap current() const;
    int selection_count() const { return int(selection_.size()); }
    QStringList fixture_types() const;
    bool can_undo() const { return document_.can_undo(); }
    bool can_redo() const { return document_.can_redo(); }
    QVariantMap layout() const;
    QVariantList universes() const;
    QVariantMap dmx_status() const;

    Q_INVOKABLE QStringList modes(const QString &gdtf) const;
    Q_INVOKABLE void selectUuid(const QString &uuid, bool add); // empty uuid clears
    // Sets a field of an object by JSON pointer relative to the object, e.g. "/position/1".
    Q_INVOKABLE void edit(const QString &uuid, const QString &pointer, const QVariant &value);
    Q_INVOKABLE void setFixtureType(const QString &uuid, const QString &gdtf); // resets the mode to the first
    Q_INVOKABLE bool setPatch(const QString &uuid, const QString &text);     // "universe.address", empty unpatches
    Q_INVOKABLE void addFixture(const QString &gdtf, const QString &mode);
    Q_INVOKABLE void addWall();
    Q_INVOKABLE void addTruss();
    Q_INVOKABLE void duplicateSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void nudgeSelection(double dx, double dy, double dz); // metres, one undo step
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void setLayout(const QString &key, const QVariant &value);
    // File actions return an error message, empty on success.
    Q_INVOKABLE void newProject();
    Q_INVOKABLE QString openProject(const QUrl &url);
    Q_INVOKABLE QString saveProject(const QUrl &url); // empty url: save to the current file
    Q_INVOKABLE QString importGdtf(const QUrl &url);

  Q_SIGNALS:
    void changed();
    void dmxChanged();

  private:
    void refresh();
    bool apply(const sv::json &patch, const std::string &label); // logs and returns false if the patch fails
    int index_of(const std::string &uuid) const;
    void add_object(sv::json object, const char *label);
    void add_gdtf(const std::string &name, std::vector<std::uint8_t> bytes); // throws on a bad file

    QString name_;
    std::filesystem::path path_;
    std::map<std::string, std::vector<std::uint8_t>> gdtf_files_; // by file name, written back into saved packages
    std::map<std::string, std::vector<std::uint8_t>> other_files_; // package entries kept as they are

    sv::Document document_;
    std::shared_ptr<const sv::json> snapshot_;
    sv::render::FixtureLibrary library_;
    std::shared_ptr<const sv::render::FixtureLibrary> library_snapshot_;
    bool library_dirty_ = true;
    sv::proto::DmxEngine dmx_;
    sv::proto::Pattern pattern_ = sv::proto::Pattern::off;
    QImage composition_;
    std::set<std::string> selection_;
    quint64 revision_ = 0;
    std::optional<std::pair<std::string, QVector3D>> moving_;
    std::vector<sv::proto::UniverseStatus> status_;
    QTimer status_timer_;
};
