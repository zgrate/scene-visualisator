#pragma once
// App state shared by QML and the viewport: document, fixture library, DMX engine, selection. Lives on the GUI
// thread; the render thread reads it only from ViewportItem's synchronize(), while the GUI thread is blocked.
#include "core/dmx_recording.h"
#include "core/document.h"
#include "core/project_file.h"
#include "media/media_source.h"
#include "media/ndi.h"
#include "protocol/dmx_engine.h"
#include "render/drawing_painter.h"
#include "render/scene_builder.h"

#include <QElapsedTimer>
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
    Q_PROPERTY(QVariantMap venue READ venue NOTIFY changed)          // width, depth, height, haze
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
    Q_PROPERTY(QVariantList feedStatus READ feed_status NOTIFY dmxChanged)
    Q_PROPERTY(QVariantMap appInfo READ app_info CONSTANT) // version, updates/crash upload support, crash folder (B7)
    Q_PROPERTY(QVariantList recentFiles READ recent_files NOTIFY changed) // {path, name, url}, newest first, existing
    Q_PROPERTY(
        QStringList missingFiles READ missing_files NOTIFY changed) // referenced but not found: types, geometry, media
    Q_PROPERTY(QStringList linkedMedia READ linked_media NOTIFY changed) // media linked by absolute path, not packed
    // AI access over MCP (docs/mcp-plan.md): {enabled, lan, files, port, token, listening, error, addresses, clients,
    // calls, lastTool, active, command}, refreshed with dmxStatus.
    Q_PROPERTY(QVariantMap aiStatus READ ai_status NOTIFY dmxChanged)
    Q_PROPERTY(bool debugMode READ debug_mode NOTIFY changed)
    // Viewport mouse and key scheme, preference "controls": "blender" (default) or "maya".
    Q_PROPERTY(QString controls READ controls NOTIFY changed)
    // Published by the render thread about twice a second: fps, ms, boxes, beams, walls, meshes, backend, device.
    Q_PROPERTY(QVariantMap renderStats READ render_stats NOTIFY dmxChanged)
    // Show recording and playback: {active, playing, paused, path, t, duration, replace, hasVideo, lossless, error,
    // fade, scopes {key: live|recording|both}}.
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY dmxChanged)
    // Signal output (preferences output*): {enabled, protocol, target, offset, rate, universes, error, ndi [{name}]}.
    Q_PROPERTY(QVariantMap outputStatus READ output_status NOTIFY dmxChanged)
    // Light control: attributes held per fixture {uuid: {attr: value}}, target groups {key, name, kind, uuids}
    Q_PROPERTY(QVariantMap programmer READ programmer NOTIFY changed)
    Q_PROPERTY(QVariantList controlGroups READ control_groups NOTIFY changed)
    Q_PROPERTY(QVariantList looks READ looks NOTIFY changed)
    Q_PROPERTY(QStringList selectedUuids READ selected_uuids NOTIFY changed)
    Q_PROPERTY(QStringList located READ located NOTIFY changed)

  public:
    explicit Session(QObject *parent); // no default: QML would construct its own instead of calling create()
    ~Session() override;
    static Session *instance(); // the one created in main()
    static QString controls() { return preference("controls", "blender").toString(); }
    static bool emulate_numpad() { return preference("emulateNumpad", false).toBool(); } // top-row digits, - and =
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
    // Output uuid -> its live feed (outputs[].feed). Sources are pooled and never deleted, so the pointers stay valid
    // on the render thread; latest() is safe there.
    std::vector<std::pair<std::string, const sv::media::MediaSource *>> feed_sources() const;
    // The take's per-output videos and how much of each shows over the live feed (scope weight × fader).
    std::vector<std::pair<std::string, const sv::media::MediaSource *>> playback_feed_sources() const;
    std::map<std::string, double> playback_opacity() const;
    QVariantList feed_status() const; // one row per output: video feed state, or a Lumiverse's universe
    // NDI senders are "MACHINE (Stream)": output uuid -> the sender whose stream is the output's deviceName.
    static std::map<std::string, std::string> match_ndi_feeds(const sv::json &outputs, const QStringList &senders);
    const std::set<std::string> &selection() const { return selection_; }
    static QVariantList to_variant_list(const sv::json &array);
    quint64 revision() const { return revision_; } // bumps on any document or selection change

    void set_project(sv::json project, QString name = QStringLiteral("Untitled"));
    void select(const std::optional<std::string> &uuid, bool add);
    void start_dmx(); // logs and carries on if a port is taken
    void set_generator(sv::proto::Pattern pattern);
    // Viewport drag: preview moves only the render snapshot; commit writes one undoable step.
    // FR-5 drag preview: fields (position, rotation, scale) replace the object's in the snapshot only; commit writes
    // them as one undo step.
    void preview_transform(const std::string &uuid, const sv::json &fields);
    void commit_transform(const std::string &label);
    void cancel_transform(); // back to the document
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
    QVariantMap venue() const;
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
    Q_INVOKABLE QString addObject(const QString &type, const QVariantMap &fields);
    // Adds one box wall per segment of points [[x, z], ...] on the floor, in one undo step.
    Q_INVOKABLE QString addWallRun(const QVariantList &points, double height, double thickness);
    // count fixtures at origin + i * step ([x, y, z], local to parentUuid when set), patched from startPatch
    // ("u.a", empty = next free) rolling to the next universe, consecutive fixture IDs, one undo step.
    Q_INVOKABLE QString addFixtureArray(const QString &gdtf, const QString &mode, int count, const QVariantList &origin,
                                        const QVariantList &step, const QString &startPatch, const QString &parentUuid);
    Q_INVOKABLE void addStrip();
    Q_INVOKABLE void duplicateSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void nudgeSelection(double dx, double dy, double dz); // metres, one undo step
    // Sets pointer (relative to each object, "/layer", "/color") on every selected object, or only those of type when
    // given, as one undo step. Returns an error string.
    Q_INVOKABLE QString editSelection(const QString &pointer, const QVariant &value, const QString &type = {});
    // Sizes the venue floor (centred on the origin) and height to everything in the scene, one undo step.
    Q_INVOKABLE QString fitVenue();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void setLayout(const QString &key, const QVariant &value);
    // File actions return an error message, empty on success.
    Q_INVOKABLE void newProject();
    Q_INVOKABLE QString openProject(const QUrl &url);
    Q_INVOKABLE QString saveProject(const QUrl &url); // empty url: save to the current file
    Q_INVOKABLE QString importGdtf(const QUrl &url);
    // FR-4 fixture type builder. fixtureSpec gives the first mode of a library type as {name, shortName, manufacturer,
    // beamAngle, fieldAngle, lumens, body [L, H, W], emitter "#rrggbb", channels [{attribute, bits, default,
    // physicalFrom, physicalTo}], bundled, warnings}; saveFixtureType writes such a spec as a project GDTF.
    Q_INVOKABLE QVariantMap fixtureSpec(const QString &gdtf) const;
    Q_INVOKABLE QString saveFixtureType(const QVariantMap &spec);
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
    // Feeds every Resolume NDI output from the sender of the same name, one undo step. Blocks about a second.
    Q_INVOKABLE QString connectResolumeFeeds();
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

    // Show recording (<base>.svdmx with the merged DMX, <base>.mp4 with the composition when a live media source
    // runs). All return the error, empty on success.
    QVariantMap recording() const;
    Q_INVOKABLE QString startRecording(const QUrl &url, bool lossless);
    Q_INVOKABLE void stopRecording();
    // Plays <base>.svdmx and <base>.mp4 beside it, looping. replace overrides live DMX, otherwise it merges.
    Q_INVOKABLE QString startPlayback(const QUrl &url);
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void pausePlayback(bool paused);
    Q_INVOKABLE void seekPlayback(double ms);
    // Chase Art-Net timecode: the take's time is the timecode minus `offset` (HH:MM:SS:FF or HH:MM:SS), held while
    // no timecode arrives, and it never loops. An empty offset keeps the last one. Returns the error for an unreadable offset.
    Q_INVOKABLE QString setPlaybackChase(bool on, const QString &offset);
    void chase_timecode(qint64 tc_ms); // one tick of the chase; -1 = no timecode (called from playback_tick)
    Q_INVOKABLE void setPlaybackReplace(bool replace); // the default scope: recording, or both (HTP)
    // What the take drives, per part of the rig: key "default", an output uuid, a controlGroups key or a fixture
    // uuid; mode "live", "recording" or "both" (HTP), empty to drop the override. Fixture beats group beats output
    // beats default. The fader (0 live .. 1 the scopes) crossfades continuous channels and switches the rest at half.
    Q_INVOKABLE void setPlaybackScope(const QString &key, const QString &mode);
    Q_INVOKABLE void setPlaybackFade(double fade);
    // A part's own fader (0..1, same keys and precedence as the scopes, independent of them); negative drops it.
    // The master fade above multiplies it.
    Q_INVOKABLE void setPlaybackScopeFade(const QString &key, double fade);
    // Per universe: 0 live .. 255 recording with each part's fader, before the master; and the slot kinds. Exposed for
    // tests.
    const std::map<std::uint16_t, sv::proto::PlaybackMix> &playback_mix();
    QVariantMap output_status() const;

    // Light control (programmer). Held attributes override live DMX and playback on their slots only; a null value
    // drops one attribute. Fixtures without an attribute ignore it. Empty uuids in releaseLights releases all.
    QVariantMap programmer() const;
    QVariantList control_groups() const;
    QStringList selected_uuids() const;
    // head >= 0 holds the attributes on that head only (programmer "heads": {"<n>": {...}}); attributes the head has no
    // channel for (the dimmer, shutter and strobe) stay fixture-wide.
    Q_INVOKABLE void setLight(const QStringList &uuids, const QVariantMap &attributes, int head = -1);
    Q_INVOKABLE void releaseLights(const QStringList &uuids);
    Q_INVOKABLE QStringList controlAttributes(const QStringList &uuids) const;    // GDTF attributes their modes have
    Q_INVOKABLE int controlHeads(const QStringList &uuids) const; // fewest heads in the selection, 0 if none
    Q_INVOKABLE QString saveGroup(const QString &name, const QStringList &uuids); // project["groups"], one undo step
    Q_INVOKABLE void deleteGroup(const QString &uuid);
    // Looks: project["looks"] [{uuid, name, programmer: {fixture uuid: attributes}}]. Recall replaces what is held.
    QVariantList looks() const; // [{uuid, name}]
    Q_INVOKABLE QString saveLook(const QString &name);
    Q_INVOKABLE void recallLook(const QString &uuid);
    Q_INVOKABLE void deleteLook(const QString &uuid);
    // Locate: holds the fixtures at full white with the shutter open; off restores what was held before.
    Q_INVOKABLE void locate(const QStringList &uuids, bool on);
    QStringList located() const;
    struct EncodedFixture {
        std::uint16_t universe = 1;
        int address = 1;
        std::vector<sv::gdtf::DmxByte> bytes;
        std::vector<std::string> unknown; // attributes the mode lacks
    };
    // GDTF attributes to DMX through the fixture's mode; opens the shutter unless a Shutter attribute is given.
    // Throws std::runtime_error for an unknown, untyped or unpatched fixture.
    EncodedFixture encode_fixture(const std::string &uuid, const sv::json &attributes, int head,
                                  bool open_shutter) const;

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
    void add_gdtf(const std::string &name, std::vector<std::uint8_t> bytes); // throws on a bad file
    // Type and mode text from another program -> {library file, mode}; guesses are added to `notes` once each.
    std::pair<std::string, std::string> resolve_type(const std::string &type, const std::string &mode,
                                                     QStringList &notes) const;
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
    void playback_tick();
    qint64 playback_time() const; // ms into the recording
    void send_playback(std::uint16_t universe, const sv::proto::Slots &data);
    void resend_playback(); // after a scope, fader or patch change
    void apply_output();    // the output* preferences to the engine and the NDI senders
    void update_ndi_out();  // one sender per fed output, sending the live feed or the take's video, whichever shows

    QString name_;
    std::filesystem::path path_;
    std::map<std::string, std::vector<std::uint8_t>> gdtf_files_; // by file name, written back into saved packages
    std::set<std::string> project_types_; // GDTF files saved with the project even when no fixture uses them
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
    std::vector<std::unique_ptr<sv::media::MediaSource>> feed_pool_;
    std::vector<sv::media::MediaSource *> idle_feeds_;
    std::map<std::string, std::pair<sv::media::MediaSource *, sv::json>> feeds_; // output uuid -> source, opened with
    sv::media::MediaSource *feed_source(const std::string &output); // the output's source, from the pool if new
    sv::media::MediaSource *pooled_source(); // sources are never destroyed: the render thread may still poll one
    std::map<std::string, sv::media::MediaSource *> playback_feeds_; // output uuid -> its recorded mp4
    std::map<std::string, std::pair<QString, std::unique_ptr<sv::media::NdiSender>>> ndi_out_; // uuid -> name, sender
    std::set<std::string> selection_;
    quint64 revision_ = 0;
    std::optional<std::pair<std::string, sv::json>> moving_;
    std::vector<sv::proto::UniverseStatus> status_;
    QTimer status_timer_;

    struct Recorder {
        std::mutex mutex; // the tap writes on the network thread, stopRecording closes on this one
        std::optional<sv::DmxRecordWriter> writer;
        QElapsedTimer clock;
    };
    std::shared_ptr<Recorder> recorder_;
    QString record_path_, record_error_;
    bool record_video_ = false;
    std::optional<sv::DmxRecording> playback_;
    QString playback_path_;
    QTimer playback_timer_;
    QElapsedTimer playback_clock_; // without a video: runs from playback_start_ms_
    qint64 playback_start_ms_ = 0;
    bool playback_paused_ = false, playback_video_ = false;
    bool chase_ = false, chase_running_ = false;
    qint64 chase_offset_ms_ = 0, chase_t_ = 0;
    std::map<std::string, std::string> playback_scopes_{{"default", "recording"}};
    double playback_fade_ = 1;
    std::map<std::string, double> playback_scope_fades_; // missing default = 1
    double scope_fade(const std::string &key) const;     // own, else the default's
    std::map<std::uint16_t, sv::proto::PlaybackMix> playback_mix_;
    bool playback_mix_dirty_ = true;
    std::vector<sv::media::MediaSource *> playback_sources_; // the take's videos; the first drives the clock
    std::map<std::uint16_t, sv::proto::Slots> playback_sent_;

    QQuickWindow *window_ = nullptr;
    std::unique_ptr<sv::mcp::Server> ai_;
    bool ai_forced_ = false;
    QString ai_error_;
    QStringList ai_addresses_;
    std::atomic<bool> ai_files_ = false, debug_ = false; // read by tool availability on the server thread
    std::string label_prefix_;
    std::map<std::uint16_t, sv::proto::Slots> ai_dmx_;
    void push_programmer();
    std::map<std::string, sv::json> programmer_; // uuid -> held attributes
    std::map<std::string, sv::json> located_;    // uuid -> the locate attributes held before Locate
    std::set<std::uint16_t> programmer_universes_;
    mutable std::mutex stats_mutex_;
    QVariantMap render_stats_;
};
