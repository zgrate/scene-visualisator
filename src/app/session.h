#pragma once
// App state shared by QML and the viewport: document, fixture library, DMX engine, selection. Lives on the GUI
// thread; the render thread reads it only from ViewportItem's synchronize(), while the GUI thread is blocked.
#include "core/document.h"
#include "core/project_file.h"
#include "media/media_source.h"
#include "protocol/dmx_engine.h"
#include "render/drawing_painter.h"
#include "render/scene_builder.h"

#include <QImage>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector3D>
#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <qqmlintegration.h>
#include <set>

class QJSEngine;
class QQmlEngine;
class QQuickWindow;
namespace sv::mcp {
class Server;
}

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
    Q_PROPERTY(QVariantMap dmxStatus READ dmx_status NOTIFY
                   dmxChanged) // {artnet, sacn: rate or -1, perUniverse: {rate, sources, lost, dropped}}
    Q_PROPERTY(QVariantList patchRows READ patch_rows NOTIFY changed)  // one row per fixture for the patch sheet
    Q_PROPERTY(int conflictCount READ conflict_count NOTIFY changed)   // fixtures overlapping another
    Q_PROPERTY(QString recoveryFile READ recovery_file NOTIFY changed) // newer autosave to offer, or empty
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)       // Arena screens and Lumiverses (B3)
    Q_PROPERTY(QVariantList layers READ layers NOTIFY changed)         // project["layers"]
    Q_PROPERTY(QVariantList drawings READ drawings NOTIFY changed)     // project["drawings"] (B6)
    Q_PROPERTY(QVariantMap compositionInfo READ composition_info NOTIFY changed)   // project["composition"]
    Q_PROPERTY(QStringList resolumeWarnings READ resolume_warnings NOTIFY changed) // from the last import
    // Composition source state (FR-13): {kind, width, height, fps, signal, error}, refreshed with dmxStatus.
    Q_PROPERTY(QVariantMap mediaStatus READ media_status NOTIFY dmxChanged)
    Q_PROPERTY(QVariantMap appInfo READ app_info CONSTANT) // version, updates/crash upload support, crash folder (B7)
    Q_PROPERTY(QVariantList recentFiles READ recent_files NOTIFY changed) // {path, name, url}, newest first, existing
    Q_PROPERTY(
        QStringList missingFiles READ missing_files NOTIFY changed) // referenced but not found: types, geometry, media
    Q_PROPERTY(QStringList linkedMedia READ linked_media NOTIFY changed) // media linked by absolute path, not packed
    // AI access over MCP (docs/mcp-plan.md): {enabled, lan, files, port, token, listening, error, addresses, clients,
    // calls, lastTool, active, command}, refreshed with dmxStatus.
    Q_PROPERTY(QVariantMap aiStatus READ ai_status NOTIFY dmxChanged)
    Q_PROPERTY(bool debugMode READ debug_mode NOTIFY changed)
    // Published by the render thread about twice a second: fps, ms, boxes, beams, walls, meshes, backend, device.
    Q_PROPERTY(QVariantMap renderStats READ render_stats NOTIFY dmxChanged)

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
    // Set-piece meshes from the package's geometry/*.obj, immutable and replaced when geometry is added.
    std::shared_ptr<const sv::render::MeshLibrary> meshes() const { return meshes_; }
    sv::render::DrawingSheet drawing_sheet(const QString &kind) const;
    sv::proto::DmxEngine &dmx() { return dmx_; }
    const QImage &composition() const { return composition_; }
    const sv::media::MediaSource &media() const { return media_; } // latest() is safe on the render thread
    QVariantMap media_status() const;
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
    // Per channel of a universe: 0 free, 1 patched, 2 claimed by two fixtures, 3 belongs to a selected fixture.
    std::array<std::uint8_t, 512> occupancy(int universe) const;
    std::filesystem::path autosave_target() const; // the project file, or an "Untitled" file in app data
    void autosave();                               // writes the autosave if there are unsaved edits

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
    QVariantList patch_rows() const;
    int conflict_count() const;
    QString recovery_file() const { return recovery_; }
    QVariantList outputs() const { return to_variant_list(document_.data().value("outputs", sv::json::array())); }
    QStringList resolume_warnings() const { return resolume_warnings_; }
    QVariantMap composition_info() const {
        return to_variant_list(sv::json::array({document_.data().value("composition", sv::json::object())}))[0].toMap();
    }

    Q_INVOKABLE QStringList modes(const QString &gdtf) const;
    Q_INVOKABLE void selectUuid(const QString &uuid, bool add); // empty uuid clears
    // Sets a field of an object or output by JSON pointer relative to it, e.g. "/position/1". The uuid
    // "composition" edits the composition.
    Q_INVOKABLE void edit(const QString &uuid, const QString &pointer, const QVariant &value);
    Q_INVOKABLE void setFixtureType(const QString &uuid, const QString &gdtf); // resets the mode to the first
    Q_INVOKABLE bool setPatch(const QString &uuid, const QString &text);       // "universe.address", empty unpatches
    Q_INVOKABLE void addFixture(const QString &gdtf, const QString &mode);
    Q_INVOKABLE void addWall();
    Q_INVOKABLE void addTruss();
    Q_INVOKABLE void addStrip();
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
    // Patches the selected fixtures one after another in fixture ID order from "universe.address" (FR-8).
    Q_INVOKABLE QString readdressSelection(const QString &start);
    Q_INVOKABLE void selectUuids(const QStringList &uuids);
    // kind: csv, ma3, resolume, mvr, obj or fbx (whole scene), objSelection or fbxSelection.
    Q_INVOKABLE QString exportFile(const QString &kind, const QUrl &url);
    // pitch: mm per composition pixel for new walls and strips; place: put new objects where the composition draws
    // them.
    Q_INVOKABLE QString importResolume(const QUrl &url, double pitch = 3.9, bool place = true);
    // Every Arena param of an object, output or "composition": {key, label, element, type, value, def, min?, max?}.
    Q_INVOKABLE QVariantList resolumeParams(const QString &uuid) const;
    // Stores the value in Arena's text form under resolume.params; export writes it into the element.
    // TargetIP must be IPv4; empty means TT_DISABLED (Arena's default output). Returns an error, empty on success.
    Q_INVOKABLE QString setResolumeParam(const QString &uuid, const QString &key, const QVariant &value);
    // "Send to Resolume": Documents/Resolume Arena/Presets/Advanced Output/<project>.xml (folder created) and whether
    // it exists, so QML can confirm the overwrite before exportFile("resolume", path). Never the live
    // AdvancedOutput.xml.
    Q_INVOKABLE QVariantMap resolumePreset() const;
    // Screens or window titles for a capture kind (FR-14).
    Q_INVOKABLE QStringList mediaSources(const QString &kind) const { return sv::media::MediaSource::available(kind); }
    // kind still or video: copies the file into media/ (linked by absolute path above 200 MB) and selects it.
    Q_INVOKABLE QString setMediaFile(const QString &kind, const QUrl &url);
    // unit: m, cm, mm, in, ft. zUp rotates the file to Y-up. split: one set piece per OBJ group (FR-3). MTL files
    // named by mtllib are copied along and colour the groups.
    Q_INVOKABLE QString importObj(const QUrl &url, const QString &unit, bool zUp, bool split);
    // FR-25: one set piece per FBX mesh node and a group per empty node, hierarchy and transforms kept, in one
    // undo step. Units and axes are converted by the reader.
    Q_INVOKABLE QString importFbx(const QUrl &url);
    // FR-19 drawings (B6): project["drawings"] entry for a kind (plan, front, side), created on first edit.
    Q_INVOKABLE QVariantMap drawing(const QString &kind) const;
    QVariantList layers() const { return list_of("layers"); }
    QVariantList drawings() const { return list_of("drawings"); }
    Q_INVOKABLE void setDrawing(const QString &kind, const QString &key, const QVariant &value); // one undo step
    Q_INVOKABLE QString exportDrawing(const QString &kind, const QUrl &url); // .pdf or .svg by suffix

    // App preferences (not the project): updateChannel ("stable"/"test", next launch), autoUpdate (kept by the
    // updater library), crashReports (opt-in uploads, B7).
    Q_INVOKABLE static QVariant preference(const QString &key, const QVariant &fallback = {});
    Q_INVOKABLE static void setPreference(const QString &key, const QVariant &value);
    Q_INVOKABLE static void checkForUpdates();
    static QVariantMap app_info();
    // FR-22: merges an MVR scene in one undo step; objects whose UUID already exists get a new one.
    Q_INVOKABLE QString importMvr(const QUrl &url);
    // FR-23: {header, rows (first 8), mapping {field: column}, fields, error} for the CSV mapping dialog.
    Q_INVOKABLE QVariantMap csvPreview(const QUrl &url) const;
    Q_INVOKABLE QString importCsv(const QUrl &url, const QVariantMap &mapping, const QString &unit, bool zUp);
    Q_INVOKABLE QString recover();      // opens the autosave in place of the saved file, unsaved
    Q_INVOKABLE void discardRecovery(); // deletes the autosave
    // "Don't save": deletes the current project's autosave, so the discarded edits are not offered back on launch.
    Q_INVOKABLE void discardChanges();
    // Copies linked media (composition.source.file outside the package) into media/, so the project travels whole.
    Q_INVOKABLE QString packMedia();
    QVariantList recent_files() const;
    QStringList missing_files() const;
    QStringList linked_media() const;

    // AI access. The tool table lives in ai_access.cpp; tools run on this (the UI) thread.
    void set_window(QQuickWindow *window) { window_ = window; }
    QQuickWindow *window() const { return window_; }
    void start_ai(bool force); // listens if the aiAccess preference is on, or force (--mcp, this run only)
    QVariantMap ai_status() const;
    bool ai_files() const { return ai_files_; }
    Q_INVOKABLE void setAiAccess(const QString &key, const QVariant &value); // enabled, lan, files, port
    Q_INVOKABLE void regenerateAiToken();
    bool debug_mode() const { return debug_; }
    Q_INVOKABLE void setDebugMode(bool on, bool persist = true); // persist: keep for the next launch
    Q_INVOKABLE QString logText(int lines, const QString &filter) const { return log_tail(lines, filter).join('\n'); }
    Q_INVOKABLE void copyText(const QString &text) const;
    static void append_log(const QString &line); // any thread; the last 2000 lines
    static QStringList log_tail(int lines, const QString &filter = {});
    void publish_render_stats(QVariantMap stats); // render thread
    QVariantMap render_stats() const;
    // Edits made while set are labelled "<prefix><label>" in the history ("AI: Delete").
    void set_label_prefix(std::string prefix) { label_prefix_ = std::move(prefix); }
    std::string try_apply(const sv::json &patch, const std::string &label); // the error, empty on success
    // type: fixture, wall, truss or strip, with the defaults of the Add buttons; fields are merged over them. One
    // undo step; the new object is selected. Returns the error, empty on success.
    std::string create_object(const std::string &type, const std::string &gdtf, const std::string &mode,
                              const sv::json &fields, std::string *uuid = nullptr);
    // DMX the AI holds per universe, merged as an internal source; values start at the address, nullopt clears.
    void set_ai_dmx(std::uint16_t universe, int address, const std::vector<int> &values);
    void clear_ai_dmx(std::optional<std::uint16_t> universe);

  Q_SIGNALS:
    void changed();
    void dmxChanged();
    void projectOpened();                // after openProject succeeds, for the missing-files warning
    void openRequested(const QUrl &url); // a file handed over by the OS (macOS Finder); QML checks unsaved edits

  private:
    void refresh();
    bool apply(const sv::json &patch, const std::string &label); // logs and returns false if the patch fails
    int index_of(const std::string &uuid) const;
    std::string path_of(const std::string &uuid) const; // "/objects/i", "/outputs/i", "/composition" or empty
    static QVariantList to_variant_list(const sv::json &array);
    void add_gdtf(const std::string &name, std::vector<std::uint8_t> bytes); // throws on a bad file
    void rebuild_meshes();
    QVariantList list_of(const char *key) const; // a top-level array of the project, empty when missing
    // Copies a geometry file into the package as geometry/<name>; a different file with that name gets "-2" etc.
    std::string store_geometry(const QFileInfo &info, const QByteArray &bytes);
    std::string store_media(const QFileInfo &info, const QByteArray &bytes); // media/<name>, same rule
    void remember_recent(const std::filesystem::path &path);
    void update_media(); // opens or reconfigures media_ when composition.source changed
    void check_recovery();
    sv::Package package() const; // the document plus the GDTF types it uses and the other package files
    void load(sv::Package pkg);
    void restart_ai(); // applies the AI preferences: stops, or (re)starts the listener
    void stop_ai();

    QString name_;
    std::filesystem::path path_;
    std::map<std::string, std::vector<std::uint8_t>> gdtf_files_;  // by file name, written back into saved packages
    std::map<std::string, std::vector<std::uint8_t>> other_files_; // package entries kept as they are

    sv::Document document_;
    std::shared_ptr<const sv::json> snapshot_;
    sv::render::FixtureLibrary library_;
    std::shared_ptr<const sv::render::FixtureLibrary> library_snapshot_;
    bool library_dirty_ = true;
    std::shared_ptr<const sv::render::MeshLibrary> meshes_ = std::make_shared<sv::render::MeshLibrary>();
    QString recovery_;
    QStringList resolume_warnings_;
    QTimer autosave_timer_;
    sv::proto::DmxEngine dmx_;
    sv::proto::Pattern pattern_ = sv::proto::Pattern::off;
    QImage composition_;
    sv::media::MediaSource media_;
    sv::json media_opened_;   // {kind, file/name} media_ was opened with
    QTemporaryDir media_dir_; // packaged videos are unpacked here, since QMediaPlayer needs a file
    std::set<std::string> selection_;
    quint64 revision_ = 0;
    std::optional<std::pair<std::string, QVector3D>> moving_;
    std::vector<sv::proto::UniverseStatus> status_;
    QTimer status_timer_;

    QQuickWindow *window_ = nullptr;
    std::unique_ptr<sv::mcp::Server> ai_;
    bool ai_forced_ = false;
    QString ai_error_;
    QStringList ai_addresses_;
    std::atomic<bool> ai_files_ = false, debug_ = false; // read by tool availability on the server thread
    std::string label_prefix_;
    std::map<std::uint16_t, sv::proto::Slots> ai_dmx_;
    mutable std::mutex stats_mutex_;
    QVariantMap render_stats_;
};
