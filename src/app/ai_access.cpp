#include "ai_access.h"

#include "session.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkInterface>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <numbers>
#include <thread>

Q_LOGGING_CATEGORY(lcMcp, "sv.mcp")

using sv::json;
using sv::mcp::Tool;

const char *const mcp_instructions =
    "Scene Visualiser: a stage lighting and LED wall previsualiser. Units are metres and degrees, Y up, +Z toward "
    "the audience. Objects (fixture, ledWall, truss, pixelStrip, setPiece, group, camera, deck, shape) have a uuid, "
    "name, "
    "position [x,y,z] and rotation [x,y,z]. Patch text is \"universe.address\" (1-based, e.g. 1.001). Every edit you "
    "make is one undo step labelled \"AI: ...\" that the user can undo. Use screenshot to see the result; get_fixture "
    "shows what DMX a fixture receives and what the renderer decodes from it. To build: add_object for single "
    "objects, add_wall_run for walls, add_fixture_array for patched rows (e.g. along a truss), and get_fixture_spec "
    "then save_fixture_type to create or edit fixture types.";

namespace {
std::atomic<bool> closing = false; // set when the Session goes; waiting tools give up instead of hanging shutdown

json to_json(const QVariant &v) {
    return json::parse(QJsonDocument(QJsonObject{{"v", QJsonValue::fromVariant(v)}}).toJson().toStdString())["v"];
}
QVariant to_variant(const json &j) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(json{{"v", j}}.dump())).object().value("v").toVariant();
}
QString qs(const json &args, const char *key, const std::string &fallback = {}) {
    return QString::fromStdString(args.value(key, fallback));
}
void fail_if(const QString &error) {
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
}
void fail_if(const std::string &error) {
    if (!error.empty())
        throw std::runtime_error(error);
}

// Runs fn on the Session's (UI) thread and waits for it; inline when already there (tests).
json on_ui(Session &s, std::function<json()> fn) {
    if (QThread::currentThread() == s.thread())
        return fn();
    auto done = std::make_shared<std::promise<json>>();
    auto result = done->get_future();
    QMetaObject::invokeMethod(
        &s,
        [done, fn = std::move(fn)] {
            try {
                done->set_value(fn());
            } catch (...) {
                done->set_exception(std::current_exception());
            }
        },
        Qt::QueuedConnection);
    for (int i = 0; i < 200; ++i) {
        if (result.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
            return result.get();
        if (closing)
            throw std::runtime_error("The visualiser is closing");
    }
    throw std::runtime_error("The visualiser did not answer within 10 s (busy, or a dialog is open)");
}

json schema(json properties = json::object(), json required = json::array()) {
    json s = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}
const json uuid_prop = {{"type", "string"}, {"description", "Object uuid"}};
const json uuids_prop = {{"type", "array"}, {"items", {{"type", "string"}}}};

const json *find_object(const json &project, const std::string &uuid) {
    for (const char *key : {"objects", "outputs"})
        if (project.contains(key))
            for (const json &o : project.at(key))
                if (o.value("uuid", std::string()) == uuid)
                    return &o;
    return nullptr;
}

std::string pointer_of(const json &project, const std::string &uuid) {
    if (uuid == "composition")
        return "/composition";
    for (const char *key : {"objects", "outputs"})
        if (project.contains(key))
            for (std::size_t i = 0; i < project.at(key).size(); ++i)
                if (project.at(key)[i].value("uuid", std::string()) == uuid)
                    return std::string("/") + key + "/" + std::to_string(i);
    throw std::runtime_error("No object, output or composition with uuid " + uuid);
}

json compact(const json &o) {
    json c = json::object();
    for (const char *key : {"uuid",  "name",      "type",  "layer",  "parent", "position", "rotation", "gdtf",
                            "mode",  "fixtureId", "patch", "output", "size",   "length",   "file",     "group",
                            "scale", "shape",     "color", "width",  "depth",  "height"})
        if (o.contains(key))
            c[key] = o.at(key);
    return c;
}

QQuickItem *viewport(Session &s) {
    QQuickItem *v = s.window() ? s.window()->findChild<QQuickItem *>("viewport") : nullptr;
    if (!v)
        throw std::runtime_error("No viewport (the app runs without a window)");
    return v;
}

QString view_name(const std::string &v) {
    QString n = QString::fromStdString(v).toLower();
    if (!QStringList{"perspective", "top", "front", "side"}.contains(n))
        throw std::runtime_error("view is perspective, top, front or side");
    n[0] = n[0].toUpper();
    return n;
}

json camera_state(QQuickItem *v) {
    QVariantMap state;
    QMetaObject::invokeMethod(v, "cameraState", Q_RETURN_ARG(QVariantMap, state));
    return to_json(state);
}

json screenshot(Session &s, const json &args) {
    QQuickWindow *w = s.window();
    if (!w)
        throw std::runtime_error("No window (the app runs without one)");
    QQuickItem *v = viewport(s);
    if (args.value("target", std::string("viewport")) == "viewport" && !v->isVisible())
        throw std::runtime_error("The 3D view is paused (another screen covers it, or 3D is switched off); switch to "
                                 "Design with 3D on, or use target window");
    std::optional<QVariantMap> saved;
    if (args.contains("view")) { // a one-off view: restored right after the grab
        QVariantMap state;
        QMetaObject::invokeMethod(v, "cameraState", Q_RETURN_ARG(QVariantMap, state));
        saved = state;
        QMetaObject::invokeMethod(v, "setPreset", Q_ARG(QString, view_name(args["view"])));
    }
    QImage image = w->grabWindow();
    if (saved)
        QMetaObject::invokeMethod(v, "setCameraState", Q_ARG(QVariantMap, *saved));
    if (image.isNull())
        throw std::runtime_error("The window could not be grabbed");
    if (args.value("target", std::string("viewport")) == "viewport") {
        const QRectF r = v->mapRectToScene(QRectF(0, 0, v->width(), v->height()));
        const qreal dpr = image.devicePixelRatio();
        image = image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect());
    }
    const int width = std::clamp(args.value("width", 1280), 160, 1920);
    if (image.width() > width)
        image = image.scaledToWidth(width, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    qCDebug(lcMcp) << "screenshot" << image.size() << png.size() << "bytes";
    return sv::mcp::image_result(png.toBase64().toStdString(),
                                 {{"width", image.width()}, {"height", image.height()}, {"camera", camera_state(v)}});
}

// Every fixture decoded from the live merged DMX, as the renderer sees it.
std::map<std::string, sv::render::FixtureState> live_states(Session &s) {
    const auto status = s.dmx().status();
    return sv::render::decode_fixtures(s.document().data(), s.library(), [&](std::uint16_t u) {
        for (const auto &st : status)
            if (st.universe == u)
                return std::span<const std::uint8_t>(st.data);
        return std::span<const std::uint8_t>();
    });
}

json rgb(const sv::gdtf::Rgb &c) { return {c[0], c[1], c[2]}; }

json fixture_state(Session &s, const std::string &uuid) {
    const json &project = s.document().data();
    const json *o = find_object(project, uuid);
    if (!o || o->value("type", std::string()) != "fixture")
        throw std::runtime_error("No fixture with uuid " + uuid);
    const auto status = s.dmx().status();
    const auto frame = [&](std::uint16_t u) {
        for (const auto &st : status)
            if (st.universe == u)
                return std::span<const std::uint8_t>(st.data);
        return std::span<const std::uint8_t>();
    };
    const auto states = live_states(s);
    json out = compact(*o);
    const auto it = states.find(uuid);
    if (it == states.end() || !it->second.type || !it->second.mode) {
        out["error"] = "Fixture type or mode not in the library";
        return out;
    }
    const auto &a = it->second.attrs;
    out["decoded"] = {{"dimmer", a.dimmer},
                      {"pan", a.pan},
                      {"tilt", a.tilt},
                      {"beamAngle", a.beam_angle},
                      {"focus", a.focus},
                      {"frost", a.frost},
                      {"iris", a.iris},
                      {"colour", rgb(a.colour)},
                      {"colourSlot", a.colour_slot},
                      {"goboSlot", a.gobo_slot},
                      {"strobeHz", a.strobe_hz},
                      {"shutterOpen", a.shutter_open},
                      {"cells", a.cells.size()}};
    json cells = json::array(), heads = json::array();
    for (const auto &c : a.cells)
        cells.push_back(rgb(c));
    for (std::size_t h = 0; h < a.heads.size(); ++h)
        heads.push_back({{"pan", a.heads[h].pan},
                         {"tilt", a.heads[h].tilt},
                         {"beamAngle", a.heads[h].beam_angle},
                         {"colour", rgb(a.heads[h].colour)},
                         {"offset", it->second.mode->heads[h].offset}});
    out["decoded"]["cellColours"] = std::move(cells);
    out["decoded"]["heads"] = std::move(heads);
    const json &patch = o->value("patch", json());
    const int u = patch.is_object() ? patch.value("universe", 1) : 0,
              address = patch.is_object() ? patch.value("address", 1) : 0;
    const auto data = frame(std::uint16_t(u));
    json channels = json::array();
    for (const auto &c : it->second.mode->channels) {
        json values = json::array();
        for (const auto off : c.offsets) {
            const int slot = address + off; // 1-based
            values.push_back(slot >= 1 && slot <= int(data.size()) ? json(data[slot - 1]) : json());
        }
        channels.push_back({{"attribute", c.attribute()},
                            {"address", address + c.offsets.front()},
                            {"dmx", values},
                            {"geometry", c.geometry},
                            {"head", c.head}});
    }
    out["channels"] = std::move(channels);
    out["universeReceiving"] = !data.empty();
    return out;
}

// shortcut: the reader keeps no geometry tree, so geometry shows only as channel geometry names and head pivots;
// add the tree when the reader stores it.
json fixture_type_json(const std::string &file, const sv::gdtf::FixtureType &t, const std::string &only_mode) {
    json modes = json::array();
    for (const auto &m : t.modes) {
        if (!only_mode.empty() && m.name != only_mode)
            continue;
        json heads = json::array(), channels = json::array();
        for (const auto &h : m.heads)
            heads.push_back({{"offset", h.offset}});
        for (const auto &c : m.channels) {
            json functions = json::array();
            for (const auto &f : c.functions) {
                json sets = json::array();
                for (const auto &cs : f.sets)
                    sets.push_back({{"dmxFrom", cs.dmx_from}, {"slot", cs.wheel_slot}});
                functions.push_back({{"name", f.name},
                                     {"attribute", f.attribute},
                                     {"dmxFrom", f.dmx_from},
                                     {"physicalFrom", f.physical_from},
                                     {"physicalTo", f.physical_to},
                                     {"wheel", f.wheel},
                                     {"sets", std::move(sets)}});
            }
            channels.push_back({{"attribute", c.attribute()},
                                {"offsets", c.offsets},
                                {"geometry", c.geometry},
                                {"head", c.head},
                                {"default", c.default_value},
                                {"functions", std::move(functions)}});
        }
        modes.push_back({{"name", m.name},
                         {"footprint", m.footprint},
                         {"heads", std::move(heads)},
                         {"channels", std::move(channels)}});
    }
    if (modes.empty())
        throw std::runtime_error("No mode named " + only_mode);
    json wheels = json::array();
    for (const auto &w : t.wheels) {
        json slots = json::array();
        for (const auto &sl : w.slots)
            slots.push_back({{"name", sl.name}, {"colour", rgb(sl.colour)}});
        wheels.push_back({{"name", w.name}, {"slots", std::move(slots)}});
    }
    return {{"gdtf", file},
            {"name", t.name},
            {"shortName", t.short_name},
            {"manufacturer", t.manufacturer},
            {"hasBeam", t.has_beam},
            {"emitter", rgb(t.emitter)},
            {"beamAngle", t.beam_angle},
            {"fieldAngle", t.field_angle},
            {"lumens", t.luminous_flux},
            {"beamRadius", t.beam_radius},
            {"body", t.body},
            {"beamDirection", t.beam_direction},
            {"wheels", std::move(wheels)},
            {"modes", std::move(modes)}};
}

json system_info(Session &s) {
    json interfaces = json::array();
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        json addresses = json::array();
        for (const QNetworkAddressEntry &e : i.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                addresses.push_back(e.ip().toString().toStdString());
        interfaces.push_back({{"name", i.humanReadableName().toStdString()},
                              {"up", bool(i.flags() & QNetworkInterface::IsUp)},
                              {"ipv4", std::move(addresses)}});
    }
    return {{"version", Session::app_info().value("version").toString().toStdString()},
            {"os", QSysInfo::prettyProductName().toStdString()},
            {"cpu", QSysInfo::currentCpuArchitecture().toStdString()},
            {"qt", qVersion()},
            {"graphics", to_json(s.render_stats().value("backend"))},
            {"device", to_json(s.render_stats().value("device"))},
            {"interfaces", std::move(interfaces)},
            {"dmx", to_json(s.dmx_status())},
            {"ndiRuntimeDir", qEnvironmentVariable("NDI_RUNTIME_DIR_V6").toStdString()},
            {"log", Session::app_info().value("crashFolder").toString().toStdString()}};
}
} // namespace

std::vector<Tool> make_mcp_tools(Session &s) {
    std::vector<Tool> tools;
    // read_only tools never edit; the others label their edits "AI: ...". args are copied for the UI-thread hop.
    const auto add = [&](std::string name, std::string description, json input, bool read_only,
                         std::function<json(const json &)> fn, std::function<bool()> available = {}) {
        tools.push_back({std::move(name), std::move(description), std::move(input), read_only,
                         [&s, fn = std::move(fn), read_only](const json &args) {
                             return on_ui(s, [&s, fn, read_only, args] {
                                 s.set_label_prefix(read_only ? "" : "AI: ");
                                 struct Reset {
                                     Session &s;
                                     ~Reset() { s.set_label_prefix({}); }
                                 } reset{s};
                                 return fn(args);
                             });
                         },
                         std::move(available)});
    };
    const auto text = [](const json &v) { return sv::mcp::text_result(v); };
    const auto files = [&s] { return s.ai_files(); };
    const auto debug = [&s] { return s.debug_mode(); };

    // Look
    add("get_project",
        "Project overview: name, file, unsaved state, object counts, universes, composition, venue (width, depth, "
        "height, haze), conflicts, missing files, selection, current screen and camera.",
        schema(), true, [&s, text](const json &) {
            const json &d = s.document().data();
            json counts = json::object();
            for (const json &o : d.at("objects"))
                counts[o.value("type", std::string("?"))] = counts.value(o.value("type", std::string("?")), 0) + 1;
            json history = json::array();
            const auto &h = s.document().history();
            for (std::size_t i = h.size() > 10 ? h.size() - 10 : 0; i < s.document().cursor(); ++i)
                history.push_back(h[i].label);
            json r = {{"name", s.project_name().toStdString()},
                      {"file", s.file_path().toStdString()},
                      {"dirty", s.dirty()},
                      {"canUndo", s.can_undo()},
                      {"canRedo", s.can_redo()},
                      {"recentEdits", history},
                      {"counts", counts},
                      {"universes", to_json(s.universes())},
                      {"composition", d.value("composition", json::object())},
                      {"venue", d.value("venue", json::object())},
                      {"patchConflicts", s.conflict_count()},
                      {"missingFiles", to_json(s.missing_files())},
                      {"linkedMedia", to_json(s.linked_media())},
                      {"recentFiles", to_json(s.recent_files())},
                      {"selection", s.selection()}};
            if (s.window()) {
                r["screen"] = s.window()->property("screen").toString().toStdString();
                r["camera"] = camera_state(viewport(s));
            }
            return text(r);
        });
    add("list_objects",
        "Objects in compact form (uuid, name, type, position, rotation, patch, ...). Every filter given must match.",
        schema({{"type", {{"type", "string"}, {"description", "Only this type, e.g. fixture, ledWall, truss"}}},
                {"gdtf", {{"type", "string"}, {"description", "Only fixtures of this type file"}}},
                {"name", {{"type", "string"}, {"description", "Name contains this, any case"}}},
                {"layer", {{"type", "string"}}},
                {"parent", {{"type", "string"}, {"description", "Parent uuid"}}},
                {"patched", {{"type", "boolean"}}}}),
        true, [&s, text](const json &args) {
            const QString name = qs(args, "name");
            json list = json::array();
            for (const json &o : s.document().data().at("objects")) {
                bool keep = true;
                for (const char *key : {"type", "gdtf", "layer", "parent"})
                    keep = keep && (!args.contains(key) || o.value(key, json()) == args[key]);
                keep =
                    keep && QString::fromStdString(o.value("name", std::string())).contains(name, Qt::CaseInsensitive);
                if (args.contains("patched"))
                    keep = keep && o.value("patch", json()).is_object() == args["patched"].get<bool>();
                if (keep)
                    list.push_back(compact(o));
            }
            return text({{"objects", list}});
        });
    add("get_object", "One object, output or \"composition\" as full JSON, the form edit_object paths refer to.",
        schema({{"uuid", uuid_prop}}, {"uuid"}), true, [&s, text](const json &args) {
            const json &d = s.document().data();
            const std::string uuid = args.at("uuid");
            return text(d.at(json::json_pointer(pointer_of(d, uuid))));
        });
    add("get_patch", "Patch sheet: one row per fixture or strip with universe, address, footprint, conflicts.",
        schema(), true, [&s, text](const json &) {
            return text({{"rows", to_json(s.patch_rows())}, {"conflicts", s.conflict_count()}});
        });
    add("get_dmx", "Live merged DMX values of a patched universe (all sources: Art-Net, sACN, generator, set_dmx).",
        schema({{"universe", {{"type", "integer"}, {"minimum", 1}}},
                {"from", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}},
                {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}},
               {"universe"}),
        true, [&s, text](const json &args) {
            const int u = args.at("universe"), from = args.value("from", 1),
                      count = std::min(args.value("count", 512), 513 - from);
            for (const auto &st : s.dmx().status())
                if (st.universe == u)
                    return text(
                        {{"universe", u},
                         {"from", from},
                         {"values", std::vector<int>(st.data.begin() + from - 1, st.data.begin() + from - 1 + count)},
                         {"rateHz", st.rate_hz},
                         {"sources", st.sources}});
            throw std::runtime_error("Universe " + std::to_string(u) +
                                     " is not patched; patched: " + to_json(s.universes()).dump());
        });
    add("get_dmx_status", "Art-Net and sACN input rates, and per universe rate, sources, lost and dropped packets.",
        schema(), true, [&s, text](const json &) { return text(to_json(s.dmx_status())); });
    add("get_media_status",
        "Composition source (kind, size, fps, signal, latency, error) and feeds: one row per Resolume output with its "
        "live feed (feedKind, feedName, fps, signal) or a Lumiverse's universe and rate.",
        schema(), true, [&s, text](const json &) {
            json st = to_json(s.media_status());
            st["feeds"] = to_json(s.feed_status());
            return text(st);
        });
    add("get_recording",
        "Show recording and playback state: active (recording), playing, paused, path, t and duration (ms), replace, "
        "hasVideo, lossless, error, fade (0 live .. 1 the scopes), scopes {key: live|recording|both}, fades {key: "
        "0..1} (each part's own fader), and output "
        "(signal output: enabled, protocol, target, offset, rate, universes, error, ndi streams).",
        schema(), true, [&s, text](const json &) {
            json st = to_json(s.recording());
            st["output"] = to_json(s.output_status());
            return text(st);
        });
    add("get_render",
        "Offline render progress (also get_recording.render): active, frame, frames, path, error, elapsedMs, rate "
        "(frames/s over the last 2 s), etaMs, queue (frames the encoder holds), stages {waitVideo, waitDmx, "
        "waitEncoder, build, draw, readback, push} (ms per frame over the last 20: the widest is the bottleneck), "
        "width, height, fps, quality, samples, backend, device, encoder, previewSerial. preview: true adds the "
        "latest rendered frame as a PNG (480 px wide).",
        schema({{"preview", {{"type", "boolean"}}}}), true, [&s, text](const json &args) {
            const json st = to_json(s.recording()["render"].toMap());
            const QImage img = s.renderPreview();
            if (!args.value("preview", false) || img.isNull())
                return text(st);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            img.save(&buffer, "PNG");
            return sv::mcp::image_result(png.toBase64().toStdString(), st);
        });
    add("get_fixture",
        "What a fixture receives and shows: its DMX per channel and the decoded attributes the "
        "renderer uses (dimmer 0..1, pan/tilt degrees, colour, beam angle, shutter).",
        schema({{"uuid", uuid_prop}}, {"uuid"}), true,
        [&s, text](const json &args) { return text(fixture_state(s, args.at("uuid"))); });
    add("list_fixture_types", "Fixture types in the library (gdtf file name, name, manufacturer, modes, footprints).",
        schema(), true, [&s, text](const json &) {
            json list = json::array();
            for (const auto &[file, t] : s.library()) {
                json modes = json::array();
                for (const auto &m : t.modes)
                    modes.push_back({{"name", m.name}, {"footprint", m.footprint}});
                list.push_back({{"gdtf", file}, {"name", t.name}, {"manufacturer", t.manufacturer}, {"modes", modes}});
            }
            return text({{"types", list}});
        });
    add("get_fixture_type",
        "A library fixture type in full: modes with channels (0-based offsets, geometry, head, functions with DMX "
        "and physical ranges, wheel sets), heads (pivot offsets, metres), emitter, beam data, hasBeam and wheels.",
        schema({{"gdtf", {{"type", "string"}}}, {"mode", {{"type", "string"}, {"description", "Only this mode"}}}},
               {"gdtf"}),
        true, [&s, text](const json &args) {
            const auto it = s.library().find(args.at("gdtf").get<std::string>());
            if (it == s.library().end())
                throw std::runtime_error("Unknown fixture type; see list_fixture_types");
            return text(fixture_type_json(it->first, it->second, args.value("mode", std::string())));
        });
    add("get_mapping",
        "Video mapping: composition (size, source), outputs, and the walls and strips with their slices.", schema(),
        true, [&s, text](const json &) {
            const json &d = s.document().data();
            json mapped = json::array();
            for (const json &o : d.at("objects"))
                if (o.contains("slice") || o.contains("output"))
                    mapped.push_back({{"uuid", o.value("uuid", std::string())},
                                      {"name", o.value("name", std::string())},
                                      {"type", o.value("type", std::string())},
                                      {"output", o.value("output", json())},
                                      {"slice", o.value("slice", json())}});
            return text({{"composition", d.value("composition", json::object())},
                         {"outputs", d.value("outputs", json::array())},
                         {"mapped", mapped}});
        });
    add("screenshot",
        "PNG of what the user sees. target viewport (3D view, default) or window (whole UI). view "
        "renders once from perspective/top/front/side and restores the camera.",
        schema({{"target", {{"type", "string"}, {"enum", {"viewport", "window"}}}},
                {"width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 1920}}},
                {"view", {{"type", "string"}, {"enum", {"perspective", "top", "front", "side"}}}}}),
        true, [&s](const json &args) { return screenshot(s, args); });

    // Act: each is one undo step
    add("add_object",
        "Adds a fixture (gdtf and mode from list_fixture_types), wall, truss, strip, deck "
        "(width, depth, height; bottom on its position) or shape (fields.shape box|cylinder|wedge, size [w,h,d], "
        "color) with the Add button defaults; fields (e.g. name, position, patch) override them. Returns the new uuid.",
        schema({{"type", {{"type", "string"}, {"enum", {"fixture", "wall", "truss", "strip", "deck", "shape"}}}},
                {"gdtf", {{"type", "string"}}},
                {"mode", {{"type", "string"}}},
                {"fields", {{"type", "object"}}}},
               {"type"}),
        false, [&s, text](const json &args) {
            std::string uuid;
            fail_if(s.create_object(args.at("type"), args.value("gdtf", std::string()),
                                    args.value("mode", std::string()), args.value("fields", json::object()), &uuid));
            return text(*find_object(s.document().data(), uuid));
        });
    add("add_wall_run",
        "Draws walls on the floor: one box shape per segment of points [[x, z], ...] (at least 2), height and "
        "thickness in metres. Returns the new wall uuids.",
        schema({{"points", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
                {"height", {{"type", "number"}}},
                {"thickness", {{"type", "number"}}}},
               {"points"}),
        false, [&s, text](const json &args) {
            fail_if(s.addWallRun(to_variant(args.at("points")).toList(), args.value("height", 3.0),
                                 args.value("thickness", 0.2)));
            return text({{"walls", s.selection()}});
        });
    add("add_fixture_array",
        "Adds count fixtures at origin + i * step ([x, y, z], local to parent when set, e.g. a truss uuid) with "
        "consecutive fixture IDs, patched from patch (\"u.a\", empty = next free) rolling to the next universe.",
        schema({{"gdtf", {{"type", "string"}}},
                {"mode", {{"type", "string"}}},
                {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}},
                {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"step", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"patch", {{"type", "string"}}},
                {"parent", {{"type", "string"}}}},
               {"gdtf", "mode", "count"}),
        false, [&s, text](const json &args) {
            fail_if(s.addFixtureArray(qs(args, "gdtf"), qs(args, "mode"), args.at("count"),
                                      to_variant(args.value("origin", json{0, 0, 0})).toList(),
                                      to_variant(args.value("step", json{1, 0, 0})).toList(), qs(args, "patch"),
                                      qs(args, "parent")));
            json fixtures = json::array();
            for (const std::string &uuid : s.selection())
                fixtures.push_back(compact(*find_object(s.document().data(), uuid)));
            std::sort(fixtures.begin(), fixtures.end(),
                      [](const json &a, const json &b) { return a.value("fixtureId", 0) < b.value("fixtureId", 0); });
            return text({{"fixtures", fixtures}});
        });
    add("get_fixture_spec",
        "A library fixture type's first mode as an editable spec: name, shortName, manufacturer, beamAngle, "
        "fieldAngle, lumens, body [L, H, W], emitter \"#rrggbb\", channels [{attribute (GDTF name), bits 8|16, "
        "default, physicalFrom, physicalTo}], plus bundled and warnings. Edit it and pass it to save_fixture_type.",
        schema({{"gdtf", {{"type", "string"}}}}, {"gdtf"}), true, [&s, text](const json &args) {
            const QVariantMap spec = s.fixtureSpec(qs(args, "gdtf"));
            if (spec.isEmpty())
                throw std::runtime_error("Unknown fixture type; see list_fixture_types");
            return text(to_json(spec));
        });
    add("save_fixture_type",
        "Creates or overwrites a project fixture type from a spec (see get_fixture_spec; needs name, manufacturer "
        "and channels). Saved as <Manufacturer>@<Name>.gdtf; bundled types are read-only, so rename a copy.",
        schema({{"spec", {{"type", "object"}}}}, {"spec"}), false, [&s, text](const json &args) {
            const json &spec = args.at("spec");
            fail_if(s.saveFixtureType(to_variant(spec).toMap()));
            for (const auto &[file, t] : s.library())
                if (t.name == spec.value("name", std::string()) &&
                    t.manufacturer == spec.value("manufacturer", std::string()) &&
                    !s.fixtureSpec(QString::fromStdString(file))["bundled"].toBool())
                    return text({{"gdtf", file}});
            return text({{"saved", true}});
        });
    add("edit_object",
        "Sets one field of an object, output or \"composition\" by JSON pointer relative to it, e.g. "
        "/position/1 or /name. Creates missing keys.",
        schema({{"uuid", uuid_prop}, {"pointer", {{"type", "string"}}}, {"value", json::object()}},
               {"uuid", "pointer", "value"}),
        false, [&s, text](const json &args) {
            const json &d = s.document().data();
            const std::string path = pointer_of(d, args.at("uuid")) + args.at("pointer").get<std::string>();
            const bool exists = d.contains(json::json_pointer(path));
            fail_if(s.try_apply(
                json::array({{{"op", exists ? "replace" : "add"}, {"path", path}, {"value", args.at("value")}}}),
                "Edit " + args.at("pointer").get<std::string>()));
            return text({{"path", path}, {"value", s.document().data().at(json::json_pointer(path))}});
        });
    add("apply_patch", "Applies an RFC 6902 JSON Patch to the whole project.json document as one undo step.",
        schema({{"patch", {{"type", "array"}, {"items", {{"type", "object"}}}}}, {"label", {{"type", "string"}}}},
               {"patch"}),
        false, [&s, text](const json &args) {
            fail_if(s.try_apply(args.at("patch"), args.value("label", std::string("Patch"))));
            return text({{"applied", args.at("patch").size()}});
        });
    add("delete_objects", "Deletes objects by uuid.", schema({{"uuids", uuids_prop}}, {"uuids"}), false,
        [&s, text](const json &args) {
            const auto before = s.document().data().at("objects").size();
            s.selectUuids(to_variant(args.at("uuids")).toStringList());
            s.deleteSelection();
            return text({{"deleted", before - s.document().data().at("objects").size()}});
        });
    add("duplicate", "Duplicates objects (next fixture ID and free address); the copies are selected.",
        schema({{"uuids", uuids_prop}}, {"uuids"}), false, [&s, text](const json &args) {
            s.selectUuids(to_variant(args.at("uuids")).toStringList());
            s.duplicateSelection();
            return text({{"copies", s.selection()}});
        });
    add("select", "Selects objects in the UI (empty list clears).", schema({{"uuids", uuids_prop}}, {"uuids"}), true,
        [&s, text](const json &args) {
            s.selectUuids(to_variant(args.at("uuids")).toStringList());
            return text({{"selection", s.selection()}});
        });
    add("set_patch", "Patches a fixture or strip at \"universe.address\" (e.g. 2.101); empty unpatches.",
        schema({{"uuid", uuid_prop}, {"patch", {{"type", "string"}}}}, {"uuid", "patch"}), false,
        [&s, text](const json &args) {
            if (!s.setPatch(qs(args, "uuid"), qs(args, "patch")))
                throw std::runtime_error("Unknown uuid or patch text (universe 1-63999, address 1-512)");
            return text(compact(*find_object(s.document().data(), args.at("uuid"))));
        });
    add("readdress", "Patches fixtures one after another in fixture ID order from a start \"universe.address\".",
        schema({{"uuids", uuids_prop}, {"start", {{"type", "string"}}}}, {"uuids", "start"}), false,
        [&s, text](const json &args) {
            s.selectUuids(to_variant(args.at("uuids")).toStringList());
            fail_if(s.readdressSelection(qs(args, "start")));
            return text({{"rows", to_json(s.patch_rows())}});
        });
    add("set_fixture_type", "Changes a fixture's type (gdtf file name); the mode resets to the type's first.",
        schema({{"uuid", uuid_prop}, {"gdtf", {{"type", "string"}}}}, {"uuid", "gdtf"}), false,
        [&s, text](const json &args) {
            if (!s.library().contains(args.at("gdtf").get<std::string>()))
                throw std::runtime_error("Unknown fixture type; see list_fixture_types");
            s.setFixtureType(qs(args, "uuid"), qs(args, "gdtf"));
            return text(compact(*find_object(s.document().data(), args.at("uuid"))));
        });
    add("undo", "Undoes the last edit (the user's or yours).", schema(), false, [&s, text](const json &) {
        if (!s.can_undo())
            throw std::runtime_error("Nothing to undo");
        const std::string label = s.document().history()[s.document().cursor() - 1].label;
        s.undo();
        return text({{"undone", label}});
    });
    add("redo", "Redoes the last undone edit.", schema(), false, [&s, text](const json &) {
        if (!s.can_redo())
            throw std::runtime_error("Nothing to redo");
        const std::string label = s.document().history()[s.document().cursor()].label;
        s.redo();
        return text({{"redone", label}});
    });
    add("set_camera",
        "Moves the 3D view: a preset (perspective, top, front, side) or an orbit camera (target "
        "[x,y,z], yaw, pitch, distance in metres, fov in degrees).",
        schema({{"preset", {{"type", "string"}, {"enum", {"perspective", "top", "front", "side"}}}},
                {"target", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"yaw", {{"type", "number"}}},
                {"pitch", {{"type", "number"}}},
                {"distance", {{"type", "number"}}},
                {"fov", {{"type", "number"}}}}),
        true, [&s, text](const json &args) {
            QQuickItem *v = viewport(s);
            if (args.contains("preset"))
                QMetaObject::invokeMethod(v, "setPreset", Q_ARG(QString, view_name(args["preset"])));
            json state = args;
            state.erase("preset");
            if (!state.empty())
                QMetaObject::invokeMethod(v, "setCameraState", Q_ARG(QVariantMap, to_variant(state).toMap()));
            return text(camera_state(v));
        });
    add("frame", "Aims the 3D view at objects' bounds, like numpad . (the user's selection is kept).",
        schema({{"uuids", uuids_prop}}, {"uuids"}), true, [&s, text](const json &args) {
            QQuickItem *v = viewport(s);
            QStringList target, before;
            for (const std::string &uuid : args.at("uuids").get<std::vector<std::string>>()) {
                if (!find_object(s.document().data(), uuid))
                    throw std::runtime_error("No object with uuid " + uuid);
                target << QString::fromStdString(uuid);
            }
            if (target.isEmpty())
                throw std::runtime_error("Give at least one uuid");
            for (const std::string &uuid : s.selection())
                before << QString::fromStdString(uuid);
            s.selectUuids(target);
            QMetaObject::invokeMethod(v, "frameSelection");
            s.selectUuids(before);
            return text(camera_state(v));
        });
    add("set_screen", "Shows a screen: design (3D), patch, dmx, plan, mapping or project.",
        schema({{"screen", {{"type", "string"}, {"enum", {"design", "patch", "dmx", "plan", "mapping", "project"}}}}},
               {"screen"}),
        true, [&s, text](const json &args) {
            if (!s.window())
                throw std::runtime_error("No window");
            const QString name = qs(args, "screen");
            if (!QStringList{"design", "patch", "dmx", "plan", "mapping", "project"}.contains(name))
                throw std::runtime_error("Unknown screen");
            s.window()->setProperty("screen", name);
            return text({{"screen", name.toStdString()}});
        });
    add("set_dmx",
        "Holds DMX values in a universe from an address on (an internal source, merged with Art-Net and "
        "sACN by priority). Stays until clear_dmx. Only patched universes are merged.",
        schema({{"universe", {{"type", "integer"}, {"minimum", 1}}},
                {"address", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}},
                {"values", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}}}}},
               {"universe", "address", "values"}),
        false, [&s, text](const json &args) {
            const int u = args.at("universe");
            if (u < 1 || u > 63999)
                throw std::runtime_error("universe is 1-63999");
            s.set_ai_dmx(std::uint16_t(u), args.at("address"), args.at("values").get<std::vector<int>>());
            const json patched = to_json(s.universes());
            return text(
                {{"held", true}, {"patched", std::find(patched.begin(), patched.end(), json(u)) != patched.end()}});
        });
    add("clear_dmx", "Releases values held by set_dmx, in one universe or all.",
        schema({{"universe", {{"type", "integer"}, {"minimum", 1}}}}), false, [&s, text](const json &args) {
            s.clear_ai_dmx(args.contains("universe") ? std::optional<std::uint16_t>(args["universe"].get<int>())
                                                     : std::nullopt);
            return text({{"cleared", args.contains("universe") ? args["universe"] : json("all")}});
        });
    add("set_fixture",
        "Sets fixtures by GDTF attribute instead of raw DMX, e.g. {\"Dimmer\": 1, \"Pan\": 30, \"ColorAdd_R\": 1, "
        "\"Gobo1\": 2}: values in physical units (Dimmer 0..1, Pan/Tilt degrees, wheels by slot number). head picks "
        "one head of a multi-head fixture (see get_fixture heads), else all. Opens the shutter unless open_shutter is "
        "false or a Shutter attribute is given. Held like set_dmx until clear_dmx.",
        schema({{"uuid", uuid_prop},
                {"uuids", uuids_prop},
                {"attributes", {{"type", "object"}}},
                {"head", {{"type", "integer"}, {"minimum", 0}}},
                {"open_shutter", {{"type", "boolean"}}}},
               {"attributes"}),
        false, [&s, text](const json &args) {
            std::vector<std::string> uuids = args.value("uuids", std::vector<std::string>());
            if (args.contains("uuid"))
                uuids.push_back(args["uuid"]);
            if (uuids.empty())
                throw std::runtime_error("Give uuid or uuids");
            const json &attributes = args.at("attributes");
            const int head = args.value("head", -1);
            json written = json::array();
            for (const std::string &uuid : uuids) {
                const auto e = s.encode_fixture(uuid, attributes, head, args.value("open_shutter", true));
                json channels = json::object();
                for (const auto &b : e.bytes) {
                    s.set_ai_dmx(e.universe, e.address + b.offset, {b.value});
                    channels[std::to_string(e.address + b.offset)] = b.value;
                }
                written.push_back(
                    {{"uuid", uuid}, {"universe", e.universe}, {"channels", channels}, {"unknown", e.unknown}});
            }
            return text({{"fixtures", written}});
        });
    json pattern_enum = json::array();
    for (const auto &[name, pattern] : sv::proto::pattern_names)
        pattern_enum.push_back(name);
    add("set_generator",
        "Runs an internal test pattern on every patched fixture: off, full, chase, rainbow, sweep, or mapping (each "
        "fixture shows the composition colour at the nearest LED wall point, so the rig follows the screens). "
        "mapping options, kept as preferences: smoothMs (transition time constant, 0 off, default 150) and max "
        "(0..1 ceiling per channel, default 1).",
        schema({{"pattern", {{"type", "string"}, {"enum", pattern_enum}}},
                {"smoothMs", {{"type", "number"}, {"minimum", 0}, {"maximum", 5000}}},
                {"max", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}},
               {"pattern"}),
        false, [&s, text](const json &args) {
            const std::string name = args.at("pattern");
            const auto it =
                std::ranges::find(sv::proto::pattern_names, name, [](const auto &p) { return std::string(p.first); });
            if (it == sv::proto::pattern_names.end())
                throw std::runtime_error("Unknown pattern");
            if (args.contains("smoothMs"))
                Session::setPreference("generatorSmoothMs", args.at("smoothMs").get<double>());
            if (args.contains("max"))
                Session::setPreference("generatorMax", args.at("max").get<double>());
            s.set_generator(it->second);
            return text({{"pattern", name},
                         {"smoothMs", Session::preference("generatorSmoothMs", 150).toDouble()},
                         {"max", Session::preference("generatorMax", 1.0).toDouble()}});
        });
    add("set_composition_source",
        "Replaces composition.source, e.g. {\"kind\":\"testPattern\"}, {\"kind\":\"grid\"}, "
        "{\"kind\":\"ndi\",\"name\":\"...\"}, {\"kind\":\"screen\",\"name\":\"...\"}.",
        schema({{"source", {{"type", "object"}}}}, {"source"}), false, [&s, text](const json &args) {
            const bool exists = s.document().data().contains(json::json_pointer("/composition/source"));
            fail_if(s.try_apply(json::array({{{"op", exists ? "replace" : "add"},
                                              {"path", "/composition/source"},
                                              {"value", args.at("source")}}}),
                                "Composition source"));
            return text(to_json(s.media_status()));
        });
    add("save_project", "Saves the project to its current file (never asks for a path).", schema(), false,
        [&s, text](const json &) {
            if (s.file_path().isEmpty())
                throw std::runtime_error("The project has no file yet; the user saves it first, or use "
                                         "save_project_as with file access on");
            fail_if(s.saveProject(QUrl()));
            return text({{"saved", s.file_path().toStdString()}});
        });

    // Files on the visualiser's machine: only with "Allow file access"
    add(
        "list_files",
        "Lists a folder on the visualiser's machine (default: the project's folder, else home), folders first. "
        "pattern is a wildcard such as *.mvr or *.svproj.",
        schema({{"path", {{"type", "string"}}}, {"pattern", {{"type", "string"}}}}), true,
        [&s, text](const json &args) {
            QString path = qs(args, "path");
            if (path.isEmpty())
                path = s.file_path().isEmpty() ? QDir::homePath() : QFileInfo(s.file_path()).absolutePath();
            const QDir dir(path);
            if (!dir.exists())
                throw std::runtime_error("No folder " + path.toStdString());
            const QStringList filter = args.contains("pattern") ? QStringList{qs(args, "pattern")} : QStringList{};
            json entries = json::array();
            const auto list = dir.entryInfoList(filter, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs,
                                                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo &f : list.mid(0, 500))
                entries.push_back({{"name", f.fileName().toStdString()},
                                   {"folder", f.isDir()},
                                   {"size", f.isDir() ? 0 : f.size()},
                                   {"modified", f.lastModified().toString(Qt::ISODate).toStdString()}});
            return text({{"path", dir.absolutePath().toStdString()},
                         {"entries", std::move(entries)},
                         {"truncated", list.size() > 500}});
        },
        files);
    add(
        "open_project",
        "Opens a .svproj by path on the visualiser's machine. Unsaved edits are lost, so it fails "
        "while the project is unsaved unless discard_changes is true.",
        schema({{"path", {{"type", "string"}}}, {"discard_changes", {{"type", "boolean"}}}}, {"path"}), false,
        [&s, text](const json &args) {
            if (s.dirty() && !args.value("discard_changes", false))
                throw std::runtime_error("The project has unsaved changes; save it, or pass discard_changes");
            fail_if(s.openProject(QUrl::fromLocalFile(qs(args, "path"))));
            return text({{"opened", s.file_path().toStdString()}, {"missingFiles", to_json(s.missing_files())}});
        },
        files);
    add(
        "save_project_as", "Saves the project as a .svproj at a path on the visualiser's machine.",
        schema({{"path", {{"type", "string"}}}}, {"path"}), false,
        [&s, text](const json &args) {
            fail_if(s.saveProject(QUrl::fromLocalFile(qs(args, "path"))));
            return text({{"saved", s.file_path().toStdString()}});
        },
        files);
    add(
        "import_file",
        "Imports a file by extension: .gdtf type, .mvr scene, .obj or .fbx set pieces, .xml Resolume "
        "Advanced Output. OBJ options: unit (m, cm, mm, in, ft), z_up, split.",
        schema({{"path", {{"type", "string"}}},
                {"unit", {{"type", "string"}}},
                {"z_up", {{"type", "boolean"}}},
                {"split", {{"type", "boolean"}}}},
               {"path"}),
        false,
        [&s, text](const json &args) {
            const QUrl url = QUrl::fromLocalFile(qs(args, "path"));
            const QString ext = qs(args, "path").section('.', -1).toLower();
            fail_if(ext == "gdtf"  ? s.importGdtf(url)
                    : ext == "mvr" ? s.importMvr(url)
                    : ext == "obj"
                        ? s.importObj(url, qs(args, "unit", "m"), args.value("z_up", false), args.value("split", false))
                    : ext == "fbx" ? s.importFbx(url)
                    : ext == "xml" ? s.importResolume(url)
                                   : QStringLiteral("Cannot import .%1 files").arg(ext));
            return text({{"imported", args.at("path")}, {"warnings", to_json(s.resolume_warnings())}});
        },
        files);
    add(
        "export_file", "Exports to a path: kind csv, ma3, resolume, mvr, obj, fbx, objSelection or fbxSelection.",
        schema({{"kind",
                 {{"type", "string"},
                  {"enum", {"csv", "ma3", "resolume", "mvr", "obj", "fbx", "objSelection", "fbxSelection"}}}},
                {"path", {{"type", "string"}}}},
               {"kind", "path"}),
        true,
        [&s, text](const json &args) {
            fail_if(s.exportFile(qs(args, "kind"), QUrl::fromLocalFile(qs(args, "path"))));
            return text({{"exported", args.at("path")}});
        },
        files);

    // Drawings (FR-19)
    const json drawing_kind = {{"type", "string"}, {"enum", {"plan", "front", "side"}}};
    add("get_drawing",
        "A plan or elevation sheet's settings: sheet (A3, A4), scale (1:N, 0 fits), title, hiddenLayers, dimensions "
        "[{a: [u, v], b: [u, v], offset?: mm}], notes [{at: [u, v], text, to?: [u, v]}], autoDimensions, labels, "
        "legend (default on) and schedule (a fixture table, "
        "default off). "
        "u and v are drawing metres: plan (x, z), front (x, -y), side (-z, -y).",
        schema({{"kind", drawing_kind}}, {"kind"}), true,
        [&s, text](const json &args) { return text(to_json(s.drawing(qs(args, "kind")))); });
    add("set_drawing",
        "Sets one key of a sheet (see get_drawing); lists such as dimensions and notes are written whole. One undo "
        "step.",
        schema({{"kind", drawing_kind},
                {"key",
                 {{"type", "string"},
                  {"enum",
                   {"sheet", "scale", "title", "hiddenLayers", "dimensions", "notes", "autoDimensions", "labels",
                    "legend", "schedule"}}}},
                {"value", json::object()}},
               {"kind", "key", "value"}),
        false, [&s, text](const json &args) {
            s.setDrawing(qs(args, "kind"), qs(args, "key"), to_variant(args.at("value")));
            return text(to_json(s.drawing(qs(args, "kind"))));
        });
    add(
        "export_drawing", "Exports a sheet to a path, PDF or SVG by its suffix.",
        schema({{"kind", drawing_kind}, {"path", {{"type", "string"}}}}, {"kind", "path"}), true,
        [&s, text](const json &args) {
            fail_if(s.exportDrawing(qs(args, "kind"), QUrl::fromLocalFile(qs(args, "path"))));
            return text({{"exported", args.at("path")}});
        },
        files);

    add(
        "record",
        "Records the show: merged DMX to <path>.svdmx, plus the composition to <path>.mp4 while a live media source "
        "runs (lossless: near-lossless 150 Mbit/s). view: true also records the 3D viewport to <path>.view.mp4 (only "
        "while it is shown). stop: true ends it.",
        schema({{"path", {{"type", "string"}}},
                {"stop", {{"type", "boolean"}}},
                {"lossless", {{"type", "boolean"}}},
                {"view", {{"type", "boolean"}}}}),
        true,
        [&s, text](const json &args) {
            if (args.value("stop", false))
                s.stopRecording();
            else if (args.contains("path"))
                fail_if(s.startRecording(QUrl::fromLocalFile(qs(args, "path")), args.value("lossless", false),
                                         args.value("view", false)));
            return text(to_json(s.recording()));
        },
        files);
    add(
        "render_video",
        "Renders the loaded take (play), else the composition video, frame by frame to an mp4: smooth at any GPU "
        "speed. Each frame seeks the videos and the take's DMX to its time, then draws the 3D view at width x height "
        "(default 1920x1080). start and duration in ms (duration defaults to the take's or video's length). camera "
        "is a list of keys [{t (ms from start), target, yaw, pitch, distance, fov}], eased between, else the current "
        "view. quality: cinematic (default: the rig and LED walls light the stage, reflections, ray-marched beams, "
        "filmic tone map, 2x supersampled) or high (the 3D view's look); samples: sub-frames per frame for motion "
        "blur (default 4 in cinematic, else 1); supersample (default true) draws cinematic frames at 2x, false is ~4x "
        "faster with softer edges. The file has no audio. Progress: get_render. stop: true ends it.",
        schema({{"path", {{"type", "string"}}},
                {"stop", {{"type", "boolean"}}},
                {"fps", {{"type", "number"}, {"minimum", 1}, {"maximum", 240}}},
                {"start", {{"type", "number"}, {"minimum", 0}}},
                {"duration", {{"type", "number"}, {"minimum", 1}}},
                {"width", {{"type", "integer"}, {"minimum", 16}}},
                {"height", {{"type", "integer"}, {"minimum", 16}}},
                {"lossless", {{"type", "boolean"}}},
                {"quality", {{"type", "string"}, {"enum", {"cinematic", "high"}}}},
                {"samples", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                {"supersample", {{"type", "boolean"}}},
                {"camera", {{"type", "array"}, {"items", {{"type", "object"}}}}}}),
        true,
        [&s, text](const json &args) {
            if (args.value("stop", false)) {
                s.stopRender();
            } else {
                if (!args.contains("path"))
                    throw std::runtime_error("Missing argument: path");
                fail_if(s.startRender(to_variant(args).toMap()));
            }
            return text(to_json(s.recording()["render"].toMap()));
        },
        files);
    add(
        "play",
        "Plays a show recording (.svdmx, with the .mp4 beside it), looping. scopes sets what the take drives per "
        "part of the rig: {key: live|recording|both|null} or {key: {mode, fade}} (fade 0..1 is that part's own fader, "
        "null drops it), key \"default\", an output uuid, a controlGroups key or a "
        "fixture uuid; fixture beats group beats output beats default; both is HTP. fade 0..1 is the master: it "
        "multiplies every part's fader (wheels and other discrete channels switch at half). replace: false is "
        "default=both. seek in ms, "
        "paused, stop: true ends it. chase: true follows incoming Art-Net timecode minus chaseOffset (HH:MM:SS:FF).",
        schema({{"path", {{"type", "string"}}},
                {"stop", {{"type", "boolean"}}},
                {"replace", {{"type", "boolean"}}},
                {"scopes", {{"type", "object"}}},
                {"fade", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"paused", {{"type", "boolean"}}},
                {"seek", {{"type", "number"}, {"minimum", 0}}},
                {"chase", {{"type", "boolean"}}},
                {"chaseOffset", {{"type", "string"}}}}),
        true,
        [&s, text](const json &args) {
            if (args.value("stop", false)) {
                s.stopPlayback();
                return text(to_json(s.recording()));
            }
            if (args.contains("replace"))
                s.setPlaybackReplace(args.at("replace"));
            if (args.contains("scopes") && args.at("scopes").is_object())
                for (const auto &[key, value] : args.at("scopes").items()) {
                    const json mode = value.is_object() ? value.value("mode", json()) : value;
                    if (!value.is_object() || value.contains("mode"))
                        s.setPlaybackScope(QString::fromStdString(key),
                                           mode.is_string() ? QString::fromStdString(mode.get<std::string>())
                                                            : QString());
                    if (value.is_object() && value.contains("fade"))
                        s.setPlaybackScopeFade(QString::fromStdString(key),
                                               value.at("fade").is_number() ? value.at("fade").get<double>() : -1);
                }
            if (args.contains("fade"))
                s.setPlaybackFade(args.at("fade"));
            if (args.contains("chase") || args.contains("chaseOffset"))
                fail_if(s.setPlaybackChase(args.value("chase", s.recording()["chase"].toBool()),
                                           QString::fromStdString(args.value("chaseOffset", std::string()))));
            if (args.contains("path"))
                fail_if(s.startPlayback(QUrl::fromLocalFile(qs(args, "path"))));
            if (args.contains("seek"))
                s.seekPlayback(args.at("seek"));
            if (args.contains("paused"))
                s.pausePlayback(args.at("paused"));
            return text(to_json(s.recording()));
        },
        files);

    add(
        "set_output",
        "Signal output (proxy): sends what the view shows (live input, the take per its scopes, held lights) as "
        "Art-Net or sACN, and each fed Resolume output as an NDI stream \"SV <name>\". target: an IP, empty for "
        "broadcast or multicast. offset is added to every universe. Saved as this machine's preferences.",
        schema({{"enabled", {{"type", "boolean"}}},
                {"protocol", {{"type", "string"}, {"enum", {"artnet", "sacn"}}}},
                {"target", {{"type", "string"}}},
                {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32767}}},
                {"priority", {{"type", "integer"}, {"minimum", 0}, {"maximum", 200}}},
                {"ndi", {{"type", "boolean"}}}}),
        true,
        [&s, text](const json &args) {
            for (const auto &[arg, key] : {std::pair{"enabled", "outputEnabled"},
                                           {"protocol", "outputProtocol"},
                                           {"target", "outputTarget"},
                                           {"offset", "outputUniverseOffset"},
                                           {"priority", "outputPriority"},
                                           {"ndi", "outputNdi"}})
                if (args.contains(arg))
                    Session::setPreference(key, to_variant(args.at(arg)));
            return text(to_json(s.output_status()));
        },
        files);

    // Debug mode only
    add(
        "get_log", "The app log, newest last: time, level, category, message. filter keeps lines containing it.",
        schema({{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}}, {"filter", {{"type", "string"}}}}),
        true,
        [text](const json &args) {
            json lines = json::array();
            for (const QString &l : Session::log_tail(args.value("lines", 200), qs(args, "filter")))
                lines.push_back(l.toStdString());
            return text({{"lines", std::move(lines)}});
        },
        debug);
    // Not through add(): a fresh wait sleeps on the server thread, since the render thread syncs with the UI thread.
    tools.push_back(
        {"get_render_stats",
         "Renderer: fps, CPU ms per frame for decode and scene build, instance counts, graphics backend and GPU. "
         "Published every 0.5 s; fresh waits (up to 3 s) for stats from frames drawn after the call.",
         schema({{"fresh", {{"type", "boolean"}}}}), true,
         [&s, text](const json &args) {
             QVariantMap stats = s.render_stats();
             bool fresh = !args.value("fresh", false);
             // The publish after next covers only frames started after this call, with the latest DMX and edits.
             const auto wanted = stats.value("serial").toULongLong() + 2;
             for (int i = 0; i < 60 && !fresh; ++i) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(50));
                 stats = s.render_stats();
                 fresh = stats.value("serial").toULongLong() >= wanted;
             }
             json r = to_json(stats);
             r["fresh"] = fresh;
             return text(r);
         },
         debug});
    add(
        "get_beams",
        "The beams the renderer builds from the live DMX: fixture, index within it, origin, direction, length "
        "(metres), what ends it (floor, ceiling, deck, shape, ledWall, setPiece, none), half angle and colour "
        "(already scaled by intensity). uuid keeps one fixture.",
        schema({{"uuid", uuid_prop}}), true,
        [&s, text](const json &args) {
            const json &project = s.document().data();
            const auto meshes = s.meshes();
            const auto scene =
                sv::render::build_scene(project, live_states(s), {}, 0, meshes ? *meshes : sv::render::MeshLibrary{});
            const auto v3 = [](const QVector3D &v) { return json{v.x(), v.y(), v.z()}; };
            std::map<int, int> index;
            json beams = json::array();
            for (const auto &b : scene.beams) {
                const json &o = project.at("objects").at(std::size_t(b.owner));
                const std::string uuid = o.value("uuid", std::string());
                const int k = index[b.owner]++;
                if (args.contains("uuid") && args["uuid"] != uuid)
                    continue;
                beams.push_back({{"uuid", uuid},
                                 {"name", o.value("name", std::string())},
                                 {"index", k},
                                 {"origin", v3(b.origin)},
                                 {"direction", v3(b.direction)},
                                 {"length", b.length},
                                 {"hit", b.hit},
                                 {"halfAngle", std::atan(b.tan_half_angle) * 180 / std::numbers::pi},
                                 {"colour", v3(b.colour)}});
            }
            return text({{"beams", std::move(beams)}});
        },
        debug);
    add(
        "get_system_info", "App version, OS, Qt, graphics backend, network interfaces, DMX input state, NDI runtime.",
        schema(), true, [&s, text](const json &) { return text(system_info(s)); }, debug);
    return tools;
}

std::vector<sv::mcp::Resource> make_mcp_resources(Session &s) {
    return {{"visualiser://project.json", "The open project (project.json)", "application/json",
             [&s] { return on_ui(s, [&s] { return json(s.document().data().dump(1)); }).get<std::string>(); }}};
}

// Session members for AI access and debug mode

void Session::start_ai(bool force) {
    ai_forced_ = ai_forced_ || force;
    ai_files_ = preference("aiFiles", false).toBool();
    restart_ai();
}

void Session::restart_ai() {
    const bool enabled = ai_forced_ || preference("aiAccess", false).toBool();
    ai_error_.clear();
    if (!enabled) {
        if (ai_)
            ai_->stop();
        Q_EMIT changed();
        return;
    }
    if (!ai_) {
        ai_ = std::make_unique<sv::mcp::Server>();
        ai_->set_info("scene-visualiser", app_info().value("version").toString().toStdString(), mcp_instructions);
        ai_->set_tools(make_mcp_tools(*this));
        ai_->set_resources(make_mcp_resources(*this));
        ai_->on_call([](const sv::mcp::Call &c) {
            std::string args = c.arguments.dump();
            if (args.size() > 200)
                args = args.substr(0, 200) + "...";
            qCInfo(lcMcp).noquote() << c.tool.c_str() << args.c_str() << QString::number(c.ms, 'f', 1) + " ms"
                                    << (c.ok ? QStringLiteral("ok") : "error: " + QString::fromStdString(c.error));
        });
    }
    QString token = preference("aiToken").toString();
    if (token.isEmpty()) {
        regenerateAiToken(); // calls back here with the new token
        return;
    }
    const bool lan = preference("aiLan", false).toBool();
    const int port = preference("aiPort", 7311).toInt();
    try {
        ai_->start({lan ? "0.0.0.0" : "127.0.0.1", std::uint16_t(port), token.toStdString()});
        qCInfo(lcMcp) << "AI access listening on" << (lan ? "all interfaces" : "127.0.0.1") << "port" << port;
    } catch (const std::exception &e) {
        ai_error_ = tr("Port %1: %2").arg(port).arg(QString::fromLocal8Bit(e.what()));
        qCWarning(lcMcp).noquote() << "AI access not started:" << ai_error_;
    }
    ai_addresses_ = {QStringLiteral("127.0.0.1")};
    if (lan) {
        ai_addresses_.clear();
        for (const QNetworkInterface &i : QNetworkInterface::allInterfaces())
            if (i.flags() & QNetworkInterface::IsUp && !(i.flags() & QNetworkInterface::IsLoopBack))
                for (const QNetworkAddressEntry &e : i.addressEntries())
                    if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                        ai_addresses_ << e.ip().toString();
    }
    Q_EMIT changed();
}

void Session::stop_ai() {
    closing = true;
    ai_.reset();
}

QVariantMap Session::ai_status_now() const {
    const auto st = ai_ ? ai_->status() : sv::mcp::Status{};
    const QString token = preference("aiToken").toString();
    const int port = st.listening ? st.port : preference("aiPort", 7311).toInt();
    const QString host = ai_addresses_.value(0, QStringLiteral("127.0.0.1"));
    return {{"enabled", ai_forced_ || preference("aiAccess", false).toBool()},
            {"lan", preference("aiLan", false)},
            {"files", bool(ai_files_)},
            {"port", port},
            {"token", token},
            {"listening", st.listening},
            {"error", ai_error_},
            {"addresses", ai_addresses_},
            {"clients", st.clients},
            {"calls", qulonglong(st.calls)},
            {"lastTool", QString::fromStdString(st.last_tool)},
            {"active", st.seconds_since_call >= 0 && st.seconds_since_call < 10},
            {"command", QStringLiteral("claude mcp add --transport http visualiser http://%1:%2/mcp --header "
                                       "\"Authorization: Bearer %3\"")
                            .arg(host)
                            .arg(port)
                            .arg(token)}};
}

void Session::setAiAccess(const QString &key, const QVariant &value) {
    static const std::map<QString, QString> keys{
        {"enabled", "aiAccess"}, {"lan", "aiLan"}, {"files", "aiFiles"}, {"port", "aiPort"}};
    const auto it = keys.find(key);
    if (it == keys.end())
        return;
    if (key == "port" && (value.toInt() < 1024 || value.toInt() > 65535))
        return;
    setPreference(it->second, value);
    if (key == "enabled" && !value.toBool())
        ai_forced_ = false;
    ai_files_ = preference("aiFiles", false).toBool();
    if (key != "files") // file access is read per call; no restart needed
        restart_ai();
    else
        Q_EMIT changed();
}

void Session::regenerateAiToken() {
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), bytes.size() / 4);
    setPreference("aiToken", QString::fromLatin1(bytes.toHex()));
    if (ai_ && (ai_forced_ || preference("aiAccess", false).toBool()))
        restart_ai(); // old token stops working at once
    else
        Q_EMIT changed();
}

void Session::setDebugMode(bool on, bool persist) {
    debug_ = on;
    if (persist)
        setPreference("debug", on);
    QLoggingCategory::setFilterRules(QLatin1StringView(encoder_log_rule) + (on ? "\nsv.*.debug=true" : ""));
    qCInfo(lcMcp) << "debug mode" << (on ? "on" : "off");
    Q_EMIT changed();
}

void Session::copyText(const QString &text) const { QGuiApplication::clipboard()->setText(text); }

namespace {
std::mutex log_mutex;
std::deque<QString> log_lines;
} // namespace

void Session::append_log(const QString &line) {
    std::lock_guard lock(log_mutex);
    log_lines.push_back(line);
    if (log_lines.size() > 2000)
        log_lines.pop_front();
}

QStringList Session::log_tail(int lines, const QString &filter) {
    std::lock_guard lock(log_mutex);
    QStringList out;
    for (auto it = log_lines.rbegin(); it != log_lines.rend() && out.size() < lines; ++it)
        if (filter.isEmpty() || it->contains(filter, Qt::CaseInsensitive))
            out.prepend(*it);
    return out;
}

void Session::publish_render_stats(QVariantMap stats) {
    std::lock_guard lock(stats_mutex_);
    stats["serial"] = render_stats_.value("serial").toULongLong() + 1; // get_render_stats fresh waits on it
    render_stats_ = std::move(stats);
}

QVariantMap Session::render_stats() const {
    std::lock_guard lock(stats_mutex_);
    return render_stats_;
}

void Session::set_ai_dmx(std::uint16_t universe, int address, const std::vector<int> &values) {
    if (address < 1 || address > 512)
        throw std::runtime_error("address is 1-512");
    if (address - 1 + int(values.size()) > 512)
        throw std::runtime_error("values run past channel 512");
    sv::proto::Slots &slots = ai_dmx_[universe]; // zeroed on first use
    for (std::size_t i = 0; i < values.size(); ++i)
        slots[address - 1 + i] = std::uint8_t(std::clamp(values[i], 0, 255));
    dmx_.set_internal_frame(universe, slots);
}

void Session::clear_ai_dmx(std::optional<std::uint16_t> universe) {
    for (auto it = ai_dmx_.begin(); it != ai_dmx_.end();)
        if (!universe || it->first == *universe) {
            dmx_.set_internal_frame(it->first, std::nullopt);
            it = ai_dmx_.erase(it);
        } else
            ++it;
}
