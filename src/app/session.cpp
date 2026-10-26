#include "session.h"

#include "crash_reports.h"
#include "updater.h"

#include "core/fbx.h"
#include "core/interchange.h"
#include "core/mvr.h"
#include "core/resolume.h"
#include "gdtf/decoder.h"
#include "gdtf/match.h"
#include "mcp/server.h"
#include "render/drawing_painter.h"
#include "render/test_pattern.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgGenerator>
#include <algorithm>
#include <climits>
#include <cmath>
#include <numbers>
#include <ranges>
#include <set>

Q_LOGGING_CATEGORY(lcSession, "sv.session")

namespace {
Session *the_session = nullptr;
using sv::json;

QVariant to_variant(const json &j) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(json{{"v", j}}.dump())).object().value("v").toVariant();
}

json from_variant(const QVariant &v) {
    return json::parse(QJsonDocument(QJsonObject{{"v", QJsonValue::fromVariant(v)}}).toJson().toStdString())["v"];
}

int footprint(const sv::render::FixtureLibrary &lib, const json &o) {
    if (o.value("type", std::string()) == "pixelStrip")
        return std::max(0, o.value("pixels", 0)) * std::max(1, o.value("rows", 1)) * sv::bytes_per_pixel(o);
    const auto it = lib.find(o.value("gdtf", std::string()));
    const sv::gdtf::Mode *m = it == lib.end() ? nullptr : it->second.mode(o.value("mode", std::string()));
    return m ? m->footprint : 0;
}

// Calls fn(universe, channel) for every DMX channel a patched object uses: a fixture's footprint from its address
// (cut at 512), a strip's pixels across the universes its Distribution rolls into.
template <class F> void for_each_channel(const sv::render::FixtureLibrary &lib, const json &o, F fn) {
    const json &p = o.value("patch", json());
    if (!p.is_object())
        return;
    if (o.value("type", std::string()) == "pixelStrip") {
        const int bpp = sv::bytes_per_pixel(o), n = footprint(lib, o) / bpp;
        for (int i = 0; i < n; ++i)
            for (const auto at = sv::strip_pixel_address(o, i); int c : std::views::iota(at.address, at.address + bpp))
                if (c >= 1 && c <= 512)
                    fn(at.universe, c);
        return;
    }
    const int u = p.value("universe", 1), a = p.value("address", 1), fp = std::max(1, footprint(lib, o));
    for (int c = std::max(a, 1); c < std::min(a + fp, 513); ++c)
        fn(u, c);
}

QString patch_text(const json &patch) {
    if (!patch.is_object())
        return {};
    return QStringLiteral("%1.%2").arg(patch.value("universe", 1)).arg(patch.value("address", 1), 3, 10, QChar('0'));
}

QString number(double v) { return QString::number(v, 'g', 4); }

std::optional<std::pair<int, int>> parse_patch(const QString &text) {
    const QStringList parts = text.trimmed().split('.');
    bool ok_u = false, ok_a = parts.size() == 2;
    const int u = parts.value(0).toInt(&ok_u), a = parts.value(1).toInt(&ok_a);
    if (!ok_u || !ok_a || u < 1 || u > 63999 || a < 1 || a > 512)
        return std::nullopt;
    return std::pair{u, a};
}

QByteArray read_file(const QUrl &url, QString &error) {
    QFile f(url.toLocalFile());
    if (!f.open(QIODevice::ReadOnly))
        error = QObject::tr("Cannot read %1").arg(f.fileName());
    return f.readAll();
}

int next_fixture_id(const json &objects) {
    int id = 100;
    for (const json &o : objects)
        if (const json &f = o.value("fixtureId", json()); f.is_number_integer())
            id = std::max(id, f.get<int>());
    return id + 1;
}

// First free address after the highest patched fixture; moves to the next universe when the footprint won't fit.
json next_patch(const json &objects, const sv::render::FixtureLibrary &lib, int fp) {
    int universe = 1, next = 1;
    for (const json &o : objects) // the channels actually used, so a strip rolling into the next universe counts
        for_each_channel(lib, o, [&](int u, int c) {
            if (u > universe || (u == universe && c + 1 > next))
                universe = u, next = c + 1;
        });
    if (next + fp - 1 > 512)
        ++universe, next = 1;
    return {{"universe", universe}, {"address", next}};
}
} // namespace

Session::Session(QObject *parent) : QObject(parent) {
    the_session = this;
    // Bundled generic types; a project package's gdtf/ entries are added on open.
    for (const QString &name : QDir(":/gdtf").entryList(QDir::Files)) {
        QFile f(":/gdtf/" + name);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = f.readAll();
        try {
            add_gdtf(name.toStdString(), {bytes.begin(), bytes.end()});
        } catch (const std::exception &e) {
            qCWarning(lcSession) << "bundled GDTF" << name << "failed:" << e.what();
        }
    }
    refresh();
    // 20 Hz is plenty for meters and status text; the viewport reads frames on the render thread instead.
    status_timer_.setInterval(50);
    connect(&status_timer_, &QTimer::timeout, this, [this] {
        status_ = dmx_.status();
        if (!pixel_targets_.empty())
            push_pixel_inputs(); // feeds and playback fades change without a document edit
        Q_EMIT dmxChanged();
    });
    status_timer_.start();
    playback_timer_.setInterval(23); // 44 Hz, the DMX refresh rate
    connect(&playback_timer_, &QTimer::timeout, this, &Session::playback_tick);
    autosave_timer_.setInterval(2 * 60 * 1000); // AT-6: at most 2 minutes of lost work
    connect(&autosave_timer_, &QTimer::timeout, this, &Session::autosave);
    autosave_timer_.start();
}

Session::~Session() {
    stop_ai();       // before anything a tool could reach
    pixel_output_.set({});
    stopRecording(); // finalises the files
    dmx_.stop();
    the_session = nullptr;
}

Session *Session::instance() { return the_session; }

Session *Session::create(QQmlEngine *, QJSEngine *) {
    QJSEngine::setObjectOwnership(the_session, QJSEngine::CppOwnership);
    return the_session;
}

void Session::set_project(sv::json project, QString name) {
    stopRecording();
    stopPlayback();
    name_ = std::move(name);
    document_ = sv::Document(std::move(project));
    selection_.clear();
    programmer_.clear();
    located_.clear();
    playback_scopes_ = {{"default", "recording"}};
    playback_fade_ = 1;
    playback_scope_fades_.clear();
    check_recovery();
    refresh();
}

void Session::select(const std::optional<std::string> &uuid, bool add) {
    if (!add)
        selection_.clear();
    if (uuid && !(add && selection_.erase(*uuid)))
        selection_.insert(*uuid);
    ++revision_;
    Q_EMIT changed();
}

void Session::start_dmx() {
    try {
        dmx_.start();
    } catch (const std::exception &e) {
        qCWarning(lcSession) << "DMX input not started:" << e.what();
    }
    apply_output();
}

void Session::set_generator(sv::proto::Pattern pattern) {
    pattern_ = pattern;
    dmx_.set_generator(pattern_, 1.0, sv::render::generator_targets(document_.data(), library_));
}

void Session::refresh() {
    std::erase_if(selection_, [&](const std::string &u) { return index_of(u) < 0; });
    snapshot_ = std::make_shared<const sv::json>(document_.data());
    if (library_dirty_)
        library_snapshot_ = std::make_shared<const sv::render::FixtureLibrary>(library_), library_dirty_ = false;
    update_media();
    std::vector<std::uint16_t> universes;
    for (const QVariant &u : this->universes())
        universes.push_back(std::uint16_t(u.toInt()));
    dmx_.subscribe(std::move(universes));
    if (pattern_ != sv::proto::Pattern::off)
        set_generator(pattern_);
    if (!programmer_.empty() || !programmer_universes_.empty())
        push_programmer(); // patch or type edits move the held bytes
    playback_mix_dirty_ = true;
    resend_playback();
    update_ddp();
    ++revision_;
    Q_EMIT changed();
}

bool Session::apply(const json &patch, const std::string &label) {
    const std::string error = try_apply(patch, label);
    if (!error.empty())
        qCWarning(lcSession) << "edit" << label.c_str() << "rejected:" << error.c_str();
    return error.empty();
}

std::string Session::try_apply(const json &patch, const std::string &label) {
    try {
        document_.apply(patch, label_prefix_ + label);
    } catch (const std::exception &e) {
        return e.what();
    }
    refresh();
    return {};
}

int Session::index_of(const std::string &uuid) const {
    const json &objects = document_.data().at("objects");
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (objects[i].value("uuid", std::string()) == uuid)
            return int(i);
    return -1;
}

void Session::preview_transform(const std::string &uuid, const json &fields) {
    const int i = index_of(uuid);
    if (i < 0)
        return;
    moving_ = {uuid, fields};
    auto copy = std::make_shared<json>(document_.data());
    for (const auto &[k, v] : fields.items())
        (*copy)["objects"][i][k] = v;
    snapshot_ = std::move(copy);
    ++revision_; // no changed(): QML models stay put until the drop
}

void Session::commit_transform(const std::string &label) {
    if (!moving_)
        return;
    const auto [uuid, fields] = *std::exchange(moving_, std::nullopt);
    const int i = index_of(uuid);
    json patch = json::array();
    for (const auto &[k, v] : fields.items())
        patch.push_back({{"op", "add"}, {"path", "/objects/" + std::to_string(i) + "/" + k}, {"value", v}});
    if (i < 0 || patch.empty() || !apply(patch, label))
        refresh();
}

void Session::cancel_transform() {
    if (std::exchange(moving_, std::nullopt))
        refresh();
}

QVariantList Session::objects() const {
    QVariantList out;
    for (const json &o : document_.data().at("objects")) {
        const std::string type = o.value("type", std::string());
        QString detail;
        if (type == "fixture")
            detail = patch_text(o.value("patch", json()));
        else if (type == "pixelStrip")
            detail = QStringLiteral("%1 px %2").arg(o.value("pixels", 0)).arg(patch_text(o.value("patch", json())));
        else if (type == "truss")
            detail = number(o.value("length", 0.0)) + " m";
        else if (type == "ledWall")
            detail = number(o.value("width", 0.0)) + QStringLiteral(" × ") + number(o.value("height", 0.0));
        else if (type == "deck")
            detail = number(o.value("width", 0.0)) + QStringLiteral(" × ") + number(o.value("depth", 0.0));
        else if (type == "shape")
            detail = QString::fromStdString(o.value("shape", std::string()));
        const std::string uuid = o.value("uuid", std::string());
        out.push_back(QVariantMap{{"uuid", QString::fromStdString(uuid)},
                                  {"name", QString::fromStdString(o.value("name", std::string()))},
                                  {"type", QString::fromStdString(type)},
                                  {"detail", detail},
                                  {"selected", selection_.contains(uuid)},
                                  {"slice", to_variant(o.value("slice", json()))},
                                  {"output", to_variant(o.value("output", json()))}});
    }
    return out;
}

QVariantMap Session::current() const {
    if (selection_.empty())
        return {};
    const int i = index_of(*selection_.begin());
    if (i < 0)
        return {};
    const json &o = document_.data().at("objects")[i];
    QVariantMap m = to_variant(o).toMap();
    const std::string type = o.value("type", std::string());
    if (type == "fixture") {
        const int fp = footprint(library_, o);
        const json &p = o.value("patch", json());
        m["footprint"] = fp;
        m["patchText"] = patch_text(p);
        m["overflow"] = p.is_object() && p.value("address", 1) + fp - 1 > 512;
        if (const auto it = library_.find(o.value("gdtf", std::string())); it != library_.end())
            m["typeName"] = QString::fromStdString(it->second.name);
    } else if (type == "pixelStrip") {
        const json &p = o.value("patch", json());
        m["footprint"] = footprint(library_, o);
        m["patchText"] = patch_text(p);
        if (p.is_object() && o.value("pixels", 0) > 0) {
            const auto last = sv::strip_pixel_address(o, footprint(library_, o) / sv::bytes_per_pixel(o) - 1);
            m["lastText"] = patch_text({{"universe", last.universe}, {"address", last.address}});
        }
    } else if (type == "ledWall") {
        const sv::WallDerived d = sv::wall_derived(o);
        m["resolution"] = QStringLiteral("%1 × %2 px · %3 × %4 modules")
                              .arg(d.pixels_x)
                              .arg(d.pixels_y)
                              .arg(d.modules_x)
                              .arg(d.modules_y);
    }
    return m;
}

QStringList Session::fixture_types() const {
    QStringList out;
    for (const auto &[file, type] : library_)
        out.push_back(QString::fromStdString(file));
    return out;
}

QStringList Session::modes(const QString &gdtf) const {
    QStringList out;
    if (const auto it = library_.find(gdtf.toStdString()); it != library_.end())
        for (const sv::gdtf::Mode &m : it->second.modes)
            out.push_back(QString::fromStdString(m.name));
    return out;
}

QVariantMap Session::layout() const { return to_variant(document_.data().value("layout", json::object())).toMap(); }

QVariantMap Session::venue() const { return to_variant(document_.data().value("venue", json::object())).toMap(); }

QVariantList Session::universes() const {
    std::set<int> u;
    for (const json &o : document_.data().at("objects"))
        for_each_channel(library_, o, [&](int universe, int) { u.insert(universe); });
    return {u.begin(), u.end()};
}

QVariantMap Session::dmx_status() const {
    double artnet = -1, sacn = -1;
    QVariantMap per;
    for (const sv::proto::UniverseStatus &st : status_) {
        QStringList sources;
        for (const std::string &src : st.sources)
            sources.push_back(QString::fromStdString(src));
        // Merged rate of the busiest universe each protocol feeds; a per-source rate needs merger stats.
        for (const std::string &id : st.source_ids)
            if (id.starts_with("artnet:"))
                artnet = std::max(artnet, st.rate_hz);
            else if (id.starts_with("sacn:"))
                sacn = std::max(sacn, st.rate_hz);
        per[QString::number(st.universe)] = QVariantMap{{"rate", st.rate_hz},
                                                        {"sources", sources},
                                                        {"lost", qulonglong(st.lost)},
                                                        {"dropped", qulonglong(st.dropped)}};
    }
    return {{"artnet", artnet}, {"sacn", sacn}, {"perUniverse", per}};
}

void Session::selectUuid(const QString &uuid, bool add) {
    select(uuid.isEmpty() ? std::nullopt : std::optional(uuid.toStdString()), add);
}

QVariantList Session::to_variant_list(const json &array) { return to_variant(array).toList(); }

std::string Session::path_of(const std::string &uuid) const {
    if (uuid == "composition")
        return "/composition";
    if (uuid == "venue")
        return "/venue";
    if (const int i = index_of(uuid); i >= 0)
        return "/objects/" + std::to_string(i);
    const json &outputs = document_.data().value("outputs", json::array());
    for (std::size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i].value("uuid", std::string()) == uuid)
            return "/outputs/" + std::to_string(i);
    return {};
}

void Session::edit(const QString &uuid, const QString &pointer, const QVariant &value) {
    const std::string base = path_of(uuid.toStdString());
    if (base.empty())
        return;
    const std::string path = base + pointer.toStdString();
    json v = from_variant(value);
    // QML numbers are doubles; keep integer fields (fixture ID, address, nits) integers in the file.
    const json::json_pointer ptr(path);
    const bool exists = document_.data().contains(ptr);
    const json old = exists ? document_.data().at(ptr) : json();
    if (old.is_number_integer() && v.is_number())
        v = std::llround(v.get<double>());
    // "add" on an array index would insert, so existing fields are replaced; "add" only creates missing keys.
    if (!exists || old != v)
        apply(json::array({{{"op", exists ? "replace" : "add"}, {"path", path}, {"value", v}}}),
              "Edit " + pointer.toStdString());
}

void Session::setFixtureType(const QString &uuid, const QString &gdtf) {
    const int i = index_of(uuid.toStdString());
    const QStringList m = modes(gdtf);
    if (i < 0 || m.isEmpty())
        return;
    const std::string base = "/objects/" + std::to_string(i);
    apply(json::array({{{"op", "replace"}, {"path", base + "/gdtf"}, {"value", gdtf.toStdString()}},
                       {{"op", "replace"}, {"path", base + "/mode"}, {"value", m.front().toStdString()}}}),
          "Change fixture type");
}

bool Session::setPatch(const QString &uuid, const QString &text) {
    const int i = index_of(uuid.toStdString());
    if (i < 0)
        return false;
    json value = nullptr;
    if (!text.trimmed().isEmpty()) {
        const auto p = parse_patch(text);
        if (!p)
            return false;
        value = {{"universe", p->first}, {"address", p->second}};
    }
    return apply(
        json::array({{{"op", "replace"}, {"path", "/objects/" + std::to_string(i) + "/patch"}, {"value", value}}}),
        "Patch");
}

std::string Session::create_object(const std::string &type, const std::string &gdtf, const std::string &mode,
                                   const json &fields, std::string *uuid) {
    const json &objects = document_.data().at("objects");
    json o;
    std::string label;
    if (type == "fixture") {
        const auto it = library_.find(gdtf);
        if (it == library_.end())
            return "Unknown fixture type " + gdtf;
        if (!it->second.mode(mode))
            return "Unknown mode " + mode + " of " + gdtf;
        const int id = next_fixture_id(objects);
        o = sv::new_fixture(it->second.name + " " + std::to_string(id), gdtf, mode);
        o["fixtureId"] = id;
        o["position"] = {0.0, 4.0, 0.0};
        o["patch"] = next_patch(objects, library_, footprint(library_, o));
        label = "Add fixture";
    } else if (type == "wall") {
        o = sv::new_led_wall("LED wall");
        o["position"] = {0.0, 1.0, 0.0};
        label = "Add LED wall";
    } else if (type == "truss") {
        o = sv::new_truss("Truss", "straight", 6);
        o["position"] = {0.0, 6.0, 0.0};
        label = "Add truss";
    } else if (type == "strip") {
        o = sv::new_pixel_strip("Pixel strip");
        o["position"] = {0.0, 3.0, 0.0};
        label = "Add pixel strip"; // patched after the fields merge, which may change the pixel count
    } else if (type == "deck") {
        o = sv::new_deck(4, 2, 1);
        label = "Add deck";
    } else if (type == "shape") {
        const std::string kind = fields.is_object() ? fields.value("shape", std::string("box")) : "box";
        o = sv::new_shape(kind == "box" ? "Box" : kind == "cylinder" ? "Cylinder" : "Wedge", kind, 1, 1, 1);
        label = "Add shape";
    } else
        return "Unknown object type " + type + " (fixture, wall, truss, strip, deck or shape)";
    if (fields.is_object()) {
        const json id = o["uuid"];
        o.merge_patch(fields);
        o["uuid"] = id;
    }
    if (type == "strip" && o["patch"].is_null())
        o["patch"] = next_patch(objects, library_, std::min(512, footprint(library_, o)));
    const std::string id = o["uuid"];
    if (std::string error =
            try_apply(json::array({{{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}}}), label);
        !error.empty())
        return error;
    select(id, false);
    if (uuid)
        *uuid = id;
    return {};
}

void Session::addFixture(const QString &gdtf, const QString &mode) {
    create_object("fixture", gdtf.toStdString(), mode.toStdString(), {});
}
void Session::addWall() { create_object("wall", {}, {}, {}); }
void Session::addStrip() { create_object("strip", {}, {}, {}); }
void Session::addTruss() { create_object("truss", {}, {}, {}); }

QString Session::addObject(const QString &type, const QVariantMap &fields) {
    json f = from_variant(fields);
    const std::string gdtf = f.value("gdtf", std::string()), mode = f.value("mode", std::string());
    f.erase("gdtf");
    f.erase("mode");
    return QString::fromStdString(create_object(type.toStdString(), gdtf, mode, f));
}

QString Session::addFixtureArray(const QString &gdtf, const QString &mode, int count, const QVariantList &origin,
                                 const QVariantList &step, const QString &startPatch, const QString &parentUuid) {
    const auto type = library_.find(gdtf.toStdString());
    if (type == library_.end())
        return tr("Unknown fixture type %1").arg(gdtf);
    if (!type->second.mode(mode.toStdString()))
        return tr("Unknown mode %1").arg(mode);
    if (count < 1 || count > 512)
        return tr("Count must be 1 to 512");
    if (origin.size() != 3 || step.size() != 3)
        return tr("Origin and step are [x, y, z]");
    const json &objects = document_.data().at("objects");
    if (!parentUuid.isEmpty() && index_of(parentUuid.toStdString()) < 0)
        return tr("Unknown parent object");
    json first = sv::new_fixture("", gdtf.toStdString(), mode.toStdString());
    const int fp = std::max(1, footprint(library_, first));
    int u = 0, a = 0;
    if (startPatch.trimmed().isEmpty()) {
        const json p = next_patch(objects, library_, fp);
        u = p["universe"], a = p["address"];
    } else if (const auto p = parse_patch(startPatch))
        std::tie(u, a) = *p;
    else
        return tr("Use universe.address, e.g. 2.001");
    const int id0 = next_fixture_id(objects);
    json patch = json::array();
    std::vector<std::string> ids;
    for (int i = 0; i < count; ++i) {
        if (a + fp - 1 > 512)
            ++u, a = 1;
        json o =
            sv::new_fixture(type->second.name + " " + std::to_string(id0 + i), gdtf.toStdString(), mode.toStdString());
        o["fixtureId"] = id0 + i;
        o["patch"] = {{"universe", u}, {"address", a}};
        o["position"] = json::array();
        for (int k = 0; k < 3; ++k)
            o["position"].push_back(origin[k].toDouble() + i * step[k].toDouble());
        if (!parentUuid.isEmpty())
            o["parent"] = parentUuid.toStdString();
        ids.push_back(o["uuid"]);
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
        a += fp;
    }
    if (std::string error = try_apply(patch, "Add " + std::to_string(count) + " fixtures"); !error.empty())
        return QString::fromStdString(error);
    QStringList list;
    for (const std::string &id : ids)
        list << QString::fromStdString(id);
    selectUuids(list);
    return {};
}

QString Session::addWallRun(const QVariantList &points, double height, double thickness) {
    if (points.size() < 2)
        return QStringLiteral("A wall needs at least two points");
    if (height <= 0 || thickness <= 0)
        return QStringLiteral("Wall height and thickness must be positive");
    json patch = json::array(), ids = json::array();
    int number = 0; // continue after the highest existing "Wall n"
    for (const json &o : document_.data().at("objects"))
        if (const std::string name = o.value("name", std::string()); name.starts_with("Wall "))
            number = std::max(number, std::atoi(name.c_str() + 5));
    for (qsizetype i = 0; i + 1 < points.size(); ++i) {
        const QVariantList a = points[i].toList(), b = points[i + 1].toList();
        if (a.size() < 2 || b.size() < 2)
            return QStringLiteral("Wall points are [x, z] pairs");
        const double dx = b[0].toDouble() - a[0].toDouble(), dz = b[1].toDouble() - a[1].toDouble();
        const double length = std::hypot(dx, dz);
        if (length < 1e-6)
            continue;
        json o = sv::new_shape("Wall " + std::to_string(++number), "box", length, height, thickness);
        o["position"] = {(a[0].toDouble() + b[0].toDouble()) / 2, 0.0, (a[1].toDouble() + b[1].toDouble()) / 2};
        o["rotation"] = {0.0, std::atan2(-dz, dx) * 180 / std::numbers::pi, 0.0};
        ids.push_back(o["uuid"]);
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
    }
    if (patch.empty())
        return QStringLiteral("All wall points are the same");
    if (std::string error = try_apply(patch, "Draw walls"); !error.empty())
        return QString::fromStdString(error);
    QStringList list;
    for (const json &id : ids)
        list << QString::fromStdString(id.get<std::string>());
    selectUuids(list);
    return {};
}

void Session::duplicateSelection() {
    json patch = json::array();
    std::set<std::string> copies;
    json objects = document_.data().at("objects"); // grows with the copies so each gets the next free ID and address
    for (const json &o : document_.data().at("objects")) {
        if (!selection_.contains(o.value("uuid", std::string())))
            continue;
        json c = o;
        c["uuid"] = sv::new_uuid();
        c["name"] = o.value("name", std::string()) + " copy";
        c["position"][0] = c["position"][0].get<double>() + 0.5;
        if (c.contains("resolume") && c["resolume"].is_object())
            c["resolume"].erase("uniqueId"); // the copy becomes its own Arena slice on export
        if (c.value("type", std::string()) == "fixture")
            c["fixtureId"] = next_fixture_id(objects);
        if (c.value("patch", json()).is_object())
            c["patch"] = next_patch(objects, library_, footprint(library_, c));
        copies.insert(c["uuid"].get<std::string>());
        objects.push_back(c);
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", c}});
    }
    if (!patch.empty() && apply(patch, "Duplicate")) {
        selection_ = copies;
        refresh();
    }
}

void Session::deleteSelection() {
    json patch = json::array();
    const json &objects = document_.data().at("objects");
    for (int i = int(objects.size()) - 1; i >= 0; --i) // back to front so earlier indices stay valid
        if (selection_.contains(objects[i].value("uuid", std::string())))
            patch.push_back({{"op", "remove"}, {"path", "/objects/" + std::to_string(i)}});
    if (!patch.empty())
        apply(patch, "Delete");
}

void Session::nudgeSelection(double dx, double dy, double dz) {
    json patch = json::array();
    const json &objects = document_.data().at("objects");
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (selection_.contains(objects[i].value("uuid", std::string()))) {
            const json &p = objects[i].at("position");
            patch.push_back({{"op", "replace"},
                             {"path", "/objects/" + std::to_string(i) + "/position"},
                             {"value", {p[0].get<double>() + dx, p[1].get<double>() + dy, p[2].get<double>() + dz}}});
        }
    if (!patch.empty())
        apply(patch, "Nudge");
}

QString Session::editSelection(const QString &pointer, const QVariant &value, const QString &type) {
    const json v = from_variant(value);
    json patch = json::array();
    const json &objects = document_.data().at("objects");
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!selection_.contains(objects[i].value("uuid", std::string())) ||
            (!type.isEmpty() && objects[i].value("type", std::string()) != type.toStdString()))
            continue;
        const json::json_pointer ptr(pointer.toStdString());
        if (!objects[i].contains(ptr) || objects[i].at(ptr) != v)
            patch.push_back({{"op", "add"}, {"path", "/objects/" + std::to_string(i) + pointer.toStdString()}, {"value", v}});
    }
    return patch.empty() ? QString() : QString::fromStdString(try_apply(patch, "Edit selection"));
}

QString Session::fitVenue() {
    const sv::render::RenderScene scene = sv::render::build_scene(document_.data(), {}, {}, 0, *meshes_);
    double x = 0, z = 0, top = 0;
    for (const auto &b : scene.pick) {
        if (b.uuid.starts_with("gizmo:"))
            continue;
        for (int c = 0; c < 8; ++c) {
            const QVector3D p = b.model.map(QVector3D(c & 1 ? 0.5f : -0.5f, c & 2 ? 0.5f : -0.5f, c & 4 ? 0.5f : -0.5f));
            x = std::max(x, double(std::abs(p.x()))), z = std::max(z, double(std::abs(p.z())));
            top = std::max(top, double(p.y()));
        }
    }
    if (x == 0 && z == 0)
        return tr("Nothing to fit");
    const auto up = [](double m) { return std::ceil(m * 2) / 2; }; // to 0.5 m
    const json &venue = document_.data().at("venue");
    return QString::fromStdString(try_apply(
        json::array({{{"op", "add"}, {"path", "/venue/width"}, {"value", up(2 * x)}},
                     {{"op", "add"}, {"path", "/venue/depth"}, {"value", up(2 * z)}},
                     {{"op", "add"}, {"path", "/venue/height"}, {"value", std::max(venue.value("height", 0.0), up(top + 0.5))}}}),
        "Fit venue"));
}

void Session::undo() {
    document_.undo();
    refresh();
}

void Session::redo() {
    document_.redo();
    refresh();
}

void Session::setLayout(const QString &key, const QVariant &value) {
    document_.layout()[key.toStdString()] = from_variant(value);
    snapshot_ = std::make_shared<const json>(document_.data());
    Q_EMIT changed();
}

void Session::add_gdtf(const std::string &name, std::vector<std::uint8_t> bytes) {
    library_.insert_or_assign(name, sv::gdtf::load_gdtf(std::span<const std::uint8_t>(bytes)));
    gdtf_files_[name] = std::move(bytes);
    library_dirty_ = true;
}

std::pair<std::string, std::string> Session::resolve_type(const std::string &type, const std::string &mode,
                                                          QStringList &notes) const {
    const sv::gdtf::Match t = sv::gdtf::match_type(library_, type);
    if (t.name.empty())
        return {{}, mode};
    const sv::gdtf::Match m = sv::gdtf::match_mode(library_.find(t.name)->second, mode);
    if (!t.exact || !m.exact) {
        const QString note = tr("\"%1\" mode \"%2\" matched to %3, mode \"%4\"; check it in the Inspector")
                                 .arg(QString::fromStdString(type), QString::fromStdString(mode),
                                      QString::fromStdString(t.name), QString::fromStdString(m.name));
        if (!notes.contains(note))
            notes.push_back(note);
    }
    return {t.name, m.name};
}

void Session::newProject() {
    path_.clear();
    other_files_.clear();
    project_types_.clear();
    rebuild_meshes();
    set_project(sv::new_project());
}

void Session::load(sv::Package pkg) {
    other_files_.clear();
    project_types_.clear();
    for (auto &[entry, bytes] : pkg.files)
        if (entry.starts_with("gdtf/")) {
            try {
                add_gdtf(entry.substr(5), std::move(bytes));
                project_types_.insert(entry.substr(5));
            } catch (const std::exception &e) {
                qCWarning(lcSession) << "GDTF" << entry.c_str() << "in package failed:" << e.what();
            }
        } else {
            other_files_[entry] = std::move(bytes);
        }
    rebuild_meshes();
    set_project(std::move(pkg.project),
                path_.empty() ? QStringLiteral("Untitled") : QString::fromStdU16String(path_.stem().u16string()));
}

QString Session::openProject(const QUrl &url) {
    const std::filesystem::path path = url.toLocalFile().toStdU16String();
    try {
        sv::Package pkg = sv::load_package(path);
        path_ = path;
        load(std::move(pkg));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    remember_recent(path);
    Q_EMIT projectOpened();
    return {};
}

sv::Package Session::package() const {
    sv::Package pkg{document_.data(), other_files_};
    for (const json &o : document_.data().at("objects")) // only the types the rig uses travel with it
        if (const auto it = gdtf_files_.find(o.value("gdtf", std::string())); it != gdtf_files_.end())
            pkg.files["gdtf/" + it->first] = it->second;
    for (const std::string &name : project_types_) // built or imported here, used or not
        if (const auto it = gdtf_files_.find(name); it != gdtf_files_.end())
            pkg.files["gdtf/" + name] = it->second;
    return pkg;
}

QString Session::saveProject(const QUrl &url) {
    std::filesystem::path path = url.isEmpty() ? path_ : std::filesystem::path(url.toLocalFile().toStdU16String());
    if (path.empty())
        return tr("No file chosen");
    if (path.extension() != ".svproj")
        path += ".svproj";
    try {
        sv::save_package(path, package());
        if (path_.empty()) // the untitled autosave is superseded by the real file
            std::filesystem::remove(sv::autosave_path(autosave_target()));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    document_.mark_saved();
    path_ = path;
    name_ = QString::fromStdU16String(path.stem().u16string());
    recovery_.clear();
    remember_recent(path);
    Q_EMIT changed();
    return {};
}

QString Session::importGdtf(const QUrl &url) {
    QFile f(url.toLocalFile());
    if (!f.open(QIODevice::ReadOnly))
        return tr("Cannot read %1").arg(f.fileName());
    const QByteArray bytes = f.readAll();
    try {
        add_gdtf(QFileInfo(f).fileName().toStdString(), {bytes.begin(), bytes.end()});
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    project_types_.insert(QFileInfo(f).fileName().toStdString());
    refresh(); // fixtures of this type re-decode with the new definition
    return {};
}

namespace {
bool is_bundled(const std::string &gdtf) { return QFile::exists(":/gdtf/" + QString::fromStdString(gdtf)); }

std::pair<double, double> default_physical(const std::string &attr) {
    if (attr == "Pan")
        return {-270, 270};
    if (attr == "Tilt")
        return {-135, 135};
    if (attr == "Zoom")
        return {5, 50};
    if (attr == "CTO")
        return {6500, 3200};
    if (attr == "Shutter1Strobe")
        return {0, 25};
    return {0, 1};
}

QString hex_colour(const sv::gdtf::Rgb &linear) {
    const auto enc = [](float v) { // linear -> sRGB transfer, 0..255
        v = std::clamp(v, 0.0f, 1.0f);
        return int(std::lround(255 * (v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f)));
    };
    return QStringLiteral("#%1%2%3")
        .arg(enc(linear[0]), 2, 16, QChar('0'))
        .arg(enc(linear[1]), 2, 16, QChar('0'))
        .arg(enc(linear[2]), 2, 16, QChar('0'));
}

sv::gdtf::Rgb linear_colour(const QColor &c) {
    const auto dec = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
    return {dec(c.redF()), dec(c.greenF()), dec(c.blueF())};
}
} // namespace

QVariantMap Session::fixtureSpec(const QString &gdtf) const {
    const auto it = library_.find(gdtf.toStdString());
    if (it == library_.end() || it->second.modes.empty())
        return {};
    const sv::gdtf::FixtureType &t = it->second;
    const sv::gdtf::Mode &m = t.modes.front();
    QStringList warnings;
    if (t.modes.size() > 1)
        warnings << tr("Only mode \"%1\" is kept; %2 other modes are dropped")
                        .arg(QString::fromStdString(m.name))
                        .arg(t.modes.size() - 1);
    if (!t.wheels.empty())
        warnings << tr("Wheels are replaced by generic 8-slot colour and gobo wheels");
    QVariantList channels;
    std::set<std::string> seen;
    bool repeated = false, wide = false;
    for (const sv::gdtf::Channel &c : m.channels) {
        if (!seen.insert(c.attribute()).second) {
            repeated = true; // pixel cells: the same attribute on several geometries
            continue;
        }
        wide |= c.offsets.size() > 2;
        const int bits = c.offsets.size() >= 2 ? 16 : 8;
        const sv::gdtf::ChannelFunction &f = c.functions.front();
        channels.push_back(QVariantMap{{"attribute", QString::fromStdString(c.attribute())},
                                       {"bits", bits},
                                       {"default", bits == 16 ? int(c.default_value) : int(c.default_value & 0xff)},
                                       {"physicalFrom", f.physical_from},
                                       {"physicalTo", c.functions.back().physical_to}});
    }
    if (repeated)
        warnings << tr("Channels repeated per cell or geometry are merged into one");
    if (wide)
        warnings << tr("Channels wider than 16 bits become 16-bit");
    return {{"name", QString::fromStdString(t.name)},
            {"shortName", QString::fromStdString(t.short_name)},
            {"manufacturer", QString::fromStdString(t.manufacturer)},
            {"beamAngle", t.beam_angle},
            {"fieldAngle", t.field_angle},
            {"lumens", t.luminous_flux},
            {"body", QVariantList{t.body[0], t.body[1], t.body[2]}},
            {"emitter", hex_colour(t.emitter)},
            {"channels", channels},
            {"bundled", is_bundled(it->first)},
            {"warnings", warnings}};
}

QString Session::saveFixtureType(const QVariantMap &spec) {
    namespace g = sv::gdtf;
    g::FixtureType t;
    t.name = spec.value("name").toString().trimmed().toStdString();
    t.manufacturer = spec.value("manufacturer").toString().trimmed().toStdString();
    t.short_name = spec.value("shortName").toString().trimmed().toStdString();
    if (t.name.empty() || t.manufacturer.empty())
        return tr("Name and manufacturer are required");
    if (t.short_name.empty())
        t.short_name = t.name;
    QString file = QString::fromStdString(t.manufacturer + "@" + t.name + ".gdtf");
    file.replace(QRegularExpression("[\\s/\\\\:*?\"<>|]"), "_");
    if (is_bundled(file.toStdString()))
        return tr("Bundled types are read-only; change the name");
    t.id = sv::new_uuid();
    t.beam_angle = spec.value("beamAngle", 25).toDouble();
    t.field_angle = spec.value("fieldAngle", t.beam_angle).toDouble();
    t.luminous_flux = spec.value("lumens", 10000).toDouble();
    if (t.beam_angle <= 0 || t.field_angle < t.beam_angle || t.luminous_flux < 0)
        return tr("Beam angle must be positive and not wider than the field angle");
    const QVariantList body = spec.value("body").toList();
    if (body.size() == 3)
        for (int i = 0; i < 3; ++i)
            t.body[std::size_t(i)] = std::max(0.01, body[i].toDouble());
    const QColor emitter(spec.value("emitter", "#ffffff").toString());
    if (!emitter.isValid())
        return tr("Emitter colour is not #rrggbb");
    t.emitter = linear_colour(emitter);

    g::Mode mode{"Default", {}, 0};
    std::uint16_t next = 0;
    std::set<std::string> seen;
    for (const QVariant &v : spec.value("channels").toList()) {
        const QVariantMap c = v.toMap();
        const std::string attr = c.value("attribute").toString().toStdString();
        if (attr.empty())
            continue;
        if (!seen.insert(attr).second)
            return tr("%1 is listed twice").arg(QString::fromStdString(attr));
        const int bits = c.value("bits", 8).toInt() == 16 ? 16 : 8;
        const std::uint32_t max = bits == 16 ? 65535 : 255;
        const int def = c.value("default", 0).toInt();
        if (def < 0 || std::uint32_t(def) > max)
            return tr("%1 default must be 0 to %2").arg(QString::fromStdString(attr)).arg(max);
        auto [from, to] = default_physical(attr);
        from = c.value("physicalFrom", from).toDouble();
        to = c.value("physicalTo", to).toDouble();
        g::Channel ch{{next}, "", std::uint32_t(def), {{attr, attr, 0, from, to, "", {}}}};
        if (bits == 16)
            ch.offsets.push_back(std::uint16_t(next + 1));
        next = std::uint16_t(next + bits / 8);
        if (attr == "Color1" || attr == "Gobo1") { // generic wheel, one slot per 32 DMX steps
            const bool colour = attr == "Color1";
            g::Wheel w{colour ? "Colors" : "Gobos", {}};
            static const std::array<std::pair<const char *, g::Rgb>, 8> colours{{{"Open", {1, 1, 1}},
                                                                                 {"Red", {1, 0, 0}},
                                                                                 {"Orange", {1, 0.4f, 0}},
                                                                                 {"Yellow", {1, 1, 0}},
                                                                                 {"Green", {0, 1, 0}},
                                                                                 {"Cyan", {0, 1, 1}},
                                                                                 {"Blue", {0, 0, 1}},
                                                                                 {"Magenta", {1, 0, 1}}}};
            auto &f = ch.functions.front();
            f.wheel = w.name, f.physical_from = 1, f.physical_to = 8;
            for (int i = 0; i < 8; ++i) {
                w.slots.push_back(colour ? g::WheelSlot{colours[std::size_t(i)].first, colours[std::size_t(i)].second}
                                         : g::WheelSlot{i ? "Gobo " + std::to_string(i) : "Open", {1, 1, 1}});
                f.sets.push_back({std::uint32_t(i * 32) << (bits - 8), i + 1});
            }
            t.wheels.push_back(std::move(w));
        }
        mode.channels.push_back(std::move(ch));
    }
    if (mode.channels.empty())
        return tr("Add at least one channel");
    if (next > 512)
        return tr("More than 512 channels");
    mode.footprint = next;
    t.modes.push_back(std::move(mode));
    try {
        const std::string xml = g::write_description(t);
        add_gdtf(file.toStdString(), sv::write_zip({{"description.xml", {xml.begin(), xml.end()}}}));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    project_types_.insert(file.toStdString());
    refresh(); // fixtures of this type re-decode with the new definition
    return {};
}

std::array<std::uint8_t, 512> Session::occupancy(int universe) const {
    std::array<std::uint8_t, 512> out{};
    for (const json &o : document_.data().at("objects")) {
        const bool sel = selection_.contains(o.value("uuid", std::string()));
        for_each_channel(library_, o, [&](int u, int c) {
            if (u != universe)
                return;
            std::uint8_t &slot = out[std::size_t(c - 1)];
            slot = sel ? 3 : slot ? 2 : 1;
        });
    }
    return out;
}

int Session::conflict_count() const {
    int n = 0;
    for (const QVariant &row : patch_rows())
        n += row.toMap().value("conflict").toBool();
    return n;
}

QVariantList Session::patch_rows() const {
    // Channel owners per universe, to flag fixtures whose range overlaps another's.
    std::map<int, std::array<int, 512>> claims;
    const json &objects = document_.data().at("objects");
    for (const json &o : objects)
        for_each_channel(library_, o, [&](int u, int ch) { ++claims[u][std::size_t(ch - 1)]; });
    QVariantList out;
    for (const json &o : objects) {
        const bool strip = o.value("type", std::string()) == "pixelStrip";
        if (!strip && o.value("type", std::string()) != "fixture")
            continue;
        const json &p = o.value("patch", json());
        const int fp = footprint(library_, o);
        bool conflict = false;
        for_each_channel(library_, o, [&](int u, int ch) { conflict |= claims[u][std::size_t(ch - 1)] > 1; });
        const auto type = library_.find(o.value("gdtf", std::string()));
        const json &id = o.value("fixtureId", json());
        const std::string uuid = o.value("uuid", std::string());
        out.push_back(
            QVariantMap{{"uuid", QString::fromStdString(uuid)},
                        {"fixtureId", id.is_number_integer() ? QVariant(id.get<int>()) : QVariant()},
                        {"name", QString::fromStdString(o.value("name", std::string()))},
                        {"type", strip                    ? tr("Pixel strip")
                                 : type == library_.end() ? QString::fromStdString(o.value("gdtf", std::string()))
                                                          : QString::fromStdString(type->second.name)},
                        {"mode", QString::fromStdString(strip ? o.value("colorFormat", std::string("rgb")) + " · " +
                                                                    std::to_string(o.value("pixels", 0)) + " px"
                                                              : o.value("mode", std::string()))},
                        {"universe", p.is_object() ? QVariant(p.value("universe", 1)) : QVariant()},
                        {"address", p.is_object() ? QVariant(p.value("address", 1)) : QVariant()},
                        {"patch", patch_text(p)},
                        {"footprint", fp},
                        {"position", to_variant(o.value("position", json::array()))},
                        {"conflict", conflict},
                        {"overflow", !strip && p.is_object() && p.value("address", 1) + fp - 1 > 512},
                        {"selected", selection_.contains(uuid)}});
    }
    return out;
}

void Session::selectUuids(const QStringList &uuids) {
    selection_.clear();
    for (const QString &u : uuids)
        selection_.insert(u.toStdString());
    ++revision_;
    Q_EMIT changed();
}

QString Session::readdressSelection(const QString &start) {
    const auto first = parse_patch(start);
    if (!first)
        return tr("Use universe.address, e.g. 2.001");
    const json &objects = document_.data().at("objects");
    std::vector<std::pair<int, std::size_t>> order; // fixture ID (unnumbered last), object index
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (objects[i].value("type", std::string()) == "fixture" &&
            selection_.contains(objects[i].value("uuid", std::string()))) {
            const json &id = objects[i].value("fixtureId", json());
            order.emplace_back(id.is_number_integer() ? id.get<int>() : INT_MAX, i);
        }
    if (order.empty())
        return tr("Select fixtures first");
    std::ranges::stable_sort(order, {}, &std::pair<int, std::size_t>::first);
    auto [u, a] = *first;
    json patch = json::array();
    for (const auto &[id, i] : order) {
        const int fp = std::max(1, footprint(library_, objects[i]));
        if (a + fp - 1 > 512)
            ++u, a = 1;
        patch.push_back({{"op", "replace"},
                         {"path", "/objects/" + std::to_string(i) + "/patch"},
                         {"value", {{"universe", u}, {"address", a}}}});
        a += fp;
    }
    apply(patch, "Re-address");
    return {};
}

QString Session::exportFile(const QString &kind, const QUrl &url) {
    std::string text;
    try {
        if (kind == "mvr") {
            const std::vector<std::uint8_t> zip = sv::mvr_export(document_.data(), package().files);
            text.assign(zip.begin(), zip.end());
        } else if (kind == "csv")
            text = sv::patch_csv(document_.data());
        else if (kind == "ma3")
            text = sv::ma3_patch_xml(document_.data());
        else if (kind == "resolume")
            text = sv::resolume_export(document_.data());
        else if (kind == "obj" || kind == "objSelection" || kind == "fbx" || kind == "fbxSelection") {
            const bool selection = kind.endsWith("Selection");
            if (selection && selection_.empty())
                return tr("Select objects first");
            const std::set<std::string> only = selection ? selection_ : std::set<std::string>();
            if (kind.startsWith("obj"))
                text = sv::render::export_obj(document_.data(), library_, *meshes_, only);
            else {
                const std::vector<std::uint8_t> fbx =
                    sv::render::export_fbx(document_.data(), library_, *meshes_, only);
                text.assign(fbx.begin(), fbx.end());
            }
        } else
            return tr("Unknown export %1").arg(kind);
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    QSaveFile f(url.toLocalFile());
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size()) ||
        !f.commit())
        return tr("Cannot write %1: %2").arg(url.toLocalFile(), f.errorString());
    return {};
}

QString Session::importResolume(const QUrl &url, double pitch, bool place) {
    QString error;
    const QByteArray xml = read_file(url, error);
    if (!error.isEmpty())
        return error;
    try {
        sv::ResolumeImportOptions options;
        options.pixel_pitch = pitch > 0 ? pitch : options.pixel_pitch;
        options.place = place;
        sv::ResolumeImport r =
            sv::resolume_import(document_.data(), {xml.constData(), std::size_t(xml.size())}, options);
        resolume_warnings_.clear();
        for (const std::string &w : r.warnings) {
            qCInfo(lcSession) << "Resolume import:" << w.c_str();
            resolume_warnings_.push_back(QString::fromStdString(w));
        }
        if (!apply(r.patch, "Import Resolume"))
            return tr("The Resolume file could not be applied, see the log");
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}

QVariantList Session::resolumeParams(const QString &uuid) const {
    const std::string path = path_of(uuid.toStdString());
    if (path.empty())
        return {};
    QVariantList out;
    try {
        for (const sv::ResolumeParam &p :
             sv::resolume_params(document_.data(), document_.data().at(json::json_pointer(path)))) {
            QVariantMap m{{"key", QString::fromStdString(p.key)},
                          {"label", QString::fromStdString(p.key).replace("~1", "/").replace("~0", "~")},
                          {"element", QString::fromStdString(p.element)},
                          {"type", QString::fromStdString(p.type)},
                          {"value", QString::fromStdString(p.value)},
                          {"def", QString::fromStdString(p.def)}};
            if (p.min)
                m["min"] = *p.min;
            if (p.max)
                m["max"] = *p.max;
            out.push_back(m);
        }
    } catch (const std::exception &e) {
        qCWarning(lcSession) << "Resolume params:" << e.what();
    }
    return out;
}

QString Session::setResolumeParam(const QString &uuid, const QString &key, const QVariant &value) {
    const std::string path = path_of(uuid.toStdString());
    if (path.empty())
        return tr("Nothing to edit");
    QVariant v = value;
    if (key.endsWith("TargetIP")) {
        const QString ip = value.toString().trimmed();
        static const QRegularExpression ipv4(R"(^((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)$)");
        if (!ip.isEmpty() && ip != "TT_DISABLED" && !ipv4.match(ip).hasMatch())
            return tr("Target IP must be an IPv4 address, or empty for Arena's default output");
        v = ip.isEmpty() ? QStringLiteral("TT_DISABLED") : ip;
    }
    const json &target = document_.data().at(json::json_pointer(path));
    json r = target.contains("resolume") && target["resolume"].is_object() ? target["resolume"] : json::object();
    const std::string text = sv::resolume_text(from_variant(v));
    if (r.contains("params") && r["params"].contains(key.toStdString()) && r["params"][key.toStdString()] == text)
        return {};
    r["params"][key.toStdString()] = text;
    apply(json::array(
              {{{"op", target.contains("resolume") ? "replace" : "add"}, {"path", path + "/resolume"}, {"value", r}}}),
          "Edit " + key.toStdString());
    return {};
}

QVariantMap Session::resolumePreset() const {
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                   "/Resolume Arena/Presets/Advanced Output");
    dir.mkpath(".");
    const QString path = dir.filePath(name_ + ".xml");
    return {{"path", path}, {"url", QUrl::fromLocalFile(path)}, {"exists", QFileInfo::exists(path)}};
}

void Session::update_media() {
    const json &comp = document_.data().value("composition", json::object());
    const json src = comp.value("source", json::object());
    const std::string kind = src.is_object() ? src.value("kind", std::string()) : std::string();
    const std::string file = src.is_object() ? src.value("file", std::string()) : std::string();
    // A package entry ("media/x.png") or a linked absolute path.
    const auto path_of_file = [&]() -> QString {
        if (const auto it = other_files_.find(file); it != other_files_.end()) {
            const QString out = media_dir_.filePath(QString::fromStdString(file).section('/', -1));
            if (QFile f(out); f.open(QIODevice::WriteOnly))
                f.write(reinterpret_cast<const char *>(it->second.data()), qint64(it->second.size()));
            return out;
        }
        return QString::fromStdString(file);
    };
    QImage still;
    if (kind == "still") {
        if (const auto it = other_files_.find(file); it != other_files_.end())
            still.loadFromData(it->second.data(), int(it->second.size()));
        else
            still.load(QString::fromStdString(file));
    }
    composition_ = sv::render::make_composition(document_.data(), still);

    const json opened = {{"kind", kind}, {"file", file}, {"name", src.is_object() ? src.value("name", "") : ""}};
    // The take's composition video holds the media source until stopPlayback.
    if (std::ranges::find(playback_sources_, &media_) != playback_sources_.end()) {
    } else if (opened != media_opened_) {
        media_opened_ = opened;
        const bool live = kind == "video" || kind == "screen" || kind == "window" || kind == "ndi" || kind == "spout" ||
                          kind == "syphon";
        media_.open(live ? QString::fromStdString(kind) : QString(),
                    kind == "video" ? path_of_file() : QString::fromStdString(opened["name"].get<std::string>()));
    }
    if (kind == "video" && src.is_object() && std::ranges::find(playback_sources_, &media_) == playback_sources_.end())
        media_.set_playback(src.value("playing", true), src.value("loop", true), src.value("rate", 1.0));

    std::map<std::string, json> wanted; // output uuid -> feed {kind, name}
    if (const json &data = document_.data(); data.contains("outputs") && data.at("outputs").is_array())
        for (const json &out : data.at("outputs"))
            if (out.is_object() && out.contains("feed") && out.at("feed").is_object() &&
                !out.at("feed").value("kind", std::string()).empty())
                wanted[out.value("uuid", std::string())] = out.at("feed");
    for (auto it = feeds_.begin(); it != feeds_.end();)
        if (!wanted.contains(it->first)) {
            it->second.first->open({}, {});
            idle_feeds_.push_back(it->second.first);
            it = feeds_.erase(it);
        } else {
            ++it;
        }
    for (const auto &[uuid, feed] : wanted)
        if (sv::media::MediaSource *source = feed_source(uuid); feeds_[uuid].second != feed) {
            feeds_[uuid].second = feed;
            source->open(QString::fromStdString(feed.value("kind", std::string())),
                         QString::fromStdString(feed.value("name", std::string())));
        }
    update_ndi_out();
}

void Session::update_ndi_out() {
    std::map<std::string, QString> wanted; // fed output uuid -> stream name
    if (preference("outputNdi", false).toBool())
        for (const json &out : document_.data().value("outputs", json::array()))
            if (const std::string uuid = out.value("uuid", std::string());
                feeds_.contains(uuid) || playback_feeds_.contains(uuid))
                wanted[uuid] = "SV " + QString::fromStdString(out.value("name", std::string()));
    std::erase_if(ndi_out_,
                  [&](const auto &e) { return !wanted.contains(e.first) || wanted[e.first] != e.second.first; });
    const auto opacity = playback_opacity();
    for (const auto &[uuid, name] : wanted) {
        auto &[stream, sender] = ndi_out_[uuid];
        if (!sender)
            stream = name, sender = std::make_unique<sv::media::NdiSender>(name);
        // shortcut: NDI out switches to the take at half the fader instead of mixing pixels; blend frames on the
        // sender thread if a show needs a video crossfade on the wire.
        const auto rec = playback_feeds_.find(uuid);
        const auto live = feeds_.find(uuid);
        sender->set_source(rec != playback_feeds_.end() && opacity.at(uuid) >= 0.5 ? rec->second
                           : live != feeds_.end()                                  ? live->second.first
                                                                                   : nullptr);
    }
}

sv::media::MediaSource *Session::pooled_source() {
    if (idle_feeds_.empty())
        return feed_pool_.emplace_back(std::make_unique<sv::media::MediaSource>()).get();
    sv::media::MediaSource *source = idle_feeds_.back();
    idle_feeds_.pop_back();
    return source;
}

sv::media::MediaSource *Session::feed_source(const std::string &output) {
    auto &slot = feeds_[output];
    if (!slot.first)
        slot.first = pooled_source();
    return slot.first;
}

std::vector<std::pair<std::string, const sv::media::MediaSource *>> Session::playback_feed_sources() const {
    return {playback_feeds_.begin(), playback_feeds_.end()};
}

double Session::scope_fade(const std::string &key) const {
    auto it = playback_scope_fades_.find(key);
    if (it == playback_scope_fades_.end())
        it = playback_scope_fades_.find("default");
    return it != playback_scope_fades_.end() ? it->second : 1.0;
}

std::map<std::string, double> Session::playback_opacity() const {
    std::map<std::string, double> out;
    for (const auto &[uuid, _] : playback_feeds_) {
        const auto it = playback_scopes_.find(uuid);
        out[uuid] = (it != playback_scopes_.end() ? it->second : playback_scopes_.at("default")) == "live"
                        ? 0.0
                        : playback_fade_ * scope_fade(uuid); // "both" counts as recording: pixel HTP is no use
    }
    return out;
}

std::vector<std::pair<std::string, const sv::media::MediaSource *>> Session::feed_sources() const {
    std::vector<std::pair<std::string, const sv::media::MediaSource *>> out;
    for (const auto &[uuid, feed] : feeds_)
        out.emplace_back(uuid, feed.first);
    return out;
}

QVariantList Session::feed_status() const {
    QVariantList rows;
    const json &data = document_.data();
    if (!data.contains("outputs") || !data.at("outputs").is_array())
        return rows;
    for (const json &out : data.at("outputs")) {
        if (!out.is_object())
            continue;
        const std::string uuid = out.value("uuid", std::string()), kind = out.value("kind", std::string());
        QVariantMap row{{"uuid", QString::fromStdString(uuid)},
                        {"name", QString::fromStdString(out.value("name", std::string()))},
                        {"kind", QString::fromStdString(kind)},
                        {"width", out.value("width", 0)},
                        {"height", out.value("height", 0)}};
        if (kind == "lumiverse") {
            const int universe = out.value("universe", 0);
            row["universe"] = universe;
            const auto it = std::ranges::find_if(status_, [&](const auto &st) { return st.universe == universe; });
            row["rate"] = it == status_.end() ? 0.0 : it->rate_hz;
            row["signal"] = it != status_.end() && it->rate_hz > 0;
        } else if (const auto it = feeds_.find(uuid); it != feeds_.end()) {
            const sv::media::MediaStatus st = it->second.first->status();
            row["feedKind"] = QString::fromStdString(it->second.second.value("kind", std::string()));
            row["feedName"] = QString::fromStdString(it->second.second.value("name", std::string()));
            row["fps"] = st.fps;
            row["signal"] = st.signal;
            row["latency"] = st.latency_ms;
            row["error"] = st.error;
        }
        rows.push_back(row);
    }
    return rows;
}

std::map<std::string, std::string> Session::match_ndi_feeds(const json &outputs, const QStringList &senders) {
    std::map<std::string, std::string> out;
    if (!outputs.is_array())
        return out;
    for (const json &o : outputs) {
        if (!o.is_object() || o.value("kind", std::string()) != "ndi")
            continue;
        const QString stream = QString::fromStdString(o.value("deviceName", o.value("name", std::string())));
        for (const QString &sender : senders) // "MACHINE (Stream)"
            if (const qsizetype open = sender.indexOf(" (");
                open >= 0 && sender.endsWith(')') && sender.mid(open + 2).chopped(1) == stream) {
                out[o.value("uuid", std::string())] = sender.toStdString();
                break;
            }
    }
    return out;
}

QString Session::connectResolumeFeeds() {
    const json &outputs = document_.data().value("outputs", json::array());
    const auto ndi = std::ranges::count_if(outputs, [](const json &o) { return o.value("kind", "") == "ndi"; });
    if (ndi == 0)
        return tr("The project has no Resolume NDI outputs; import the Advanced Output preset first");
    const auto matches = match_ndi_feeds(outputs, sv::media::MediaSource::available("ndi"));
    json patch = json::array();
    for (std::size_t i = 0; i < outputs.size(); ++i)
        if (const auto it = matches.find(outputs[i].value("uuid", std::string())); it != matches.end())
            patch.push_back({{"op", "add"},
                             {"path", "/outputs/" + std::to_string(i) + "/feed"},
                             {"value", {{"kind", "ndi"}, {"name", it->second}}}});
    if (!patch.empty() && apply(patch, "Connect Resolume NDI"))
        refresh();
    return tr("%1 of %2 NDI outputs connected").arg(matches.size()).arg(ndi);
}

QVariantMap Session::media_status() const {
    const sv::media::MediaStatus st = media_.status();
    const json &comp = document_.data().value("composition", json::object());
    const json &src = comp.value("source", json::object());
    return {{"kind", QString::fromStdString(src.is_object() ? src.value("kind", std::string()) : std::string())},
            {"width", st.size.width()},
            {"height", st.size.height()},
            {"fps", st.fps},
            {"signal", st.signal},
            {"latency", st.latency_ms},
            {"live", media_.active()},
            {"error", st.error}};
}

QVariantMap Session::recording() const {
    return {{"active", bool(recorder_)},
            {"playing", bool(playback_)},
            {"paused", playback_paused_},
            {"path", recorder_ ? record_path_ : playback_path_},
            {"t", recorder_ ? recorder_->clock.elapsed() : playback_time()},
            {"duration", playback_ ? qint64(playback_->duration_ms()) : 0},
            {"replace", playback_scopes_.at("default") == "recording"},
            {"fade", playback_fade_},
            {"chase", chase_},
            {"chaseOffset", chase_offset_ms_},
            {"timecode",
             [this]() -> QVariant {
                 const sv::proto::Timecode tc = dmx_.timecode();
                 if (tc.ms < 0)
                     return QString();
                 const qint64 fps = qRound(tc.fps), s = tc.ms / 1000;
                 return QString::asprintf("%02lld:%02lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60,
                                          tc.ms % 1000 * fps / 1000);
             }()},
            {"scopes",
             [this] {
                 QVariantMap m;
                 for (const auto &[key, mode] : playback_scopes_)
                     m[QString::fromStdString(key)] = QString::fromStdString(mode);
                 return m;
             }()},
            {"fades",
             [this] {
                 QVariantMap m;
                 for (const auto &[key, fade] : playback_scope_fades_)
                     m[QString::fromStdString(key)] = fade;
                 return m;
             }()},
            {"hasVideo", recorder_ ? record_video_ : playback_video_},
            {"lossless", preference("recordLossless", false).toBool()}, // the registry stores text
            {"error", record_error_}};
}

namespace {
QString recording_base(QString path) { // the path without .svdmx
    if (path.endsWith(".svdmx", Qt::CaseInsensitive))
        path.chop(6);
    return path;
}

QString feed_file_part(QString name) { // an output name made safe for "<base>.<name>.mp4"
    for (QChar &c : name)
        if (!c.isLetterOrNumber() && c != ' ' && c != '-' && c != '_')
            c = '_';
    return name;
}
} // namespace

QString Session::startRecording(const QUrl &url, bool lossless) {
    if (playback_)
        return tr("Stop playback before recording");
    stopRecording();
    const QString base = recording_base(url.toLocalFile());
    auto rec = std::make_shared<Recorder>();
    try {
        rec->writer.emplace(std::filesystem::path((base + ".svdmx").toStdU16String()));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    setPreference("recordLossless", lossless);
    for (const auto &st : dmx_.status()) // the state at t = 0, for universes that hold still
        rec->writer->write(0, st.universe, st.data);
    rec->clock.start();
    // shortcut: the tap writes and deflates on the network thread (about a ms per second of show); move it to a
    // writer thread if packet loss shows while recording.
    dmx_.set_tap([rec](std::uint16_t universe, const sv::proto::Slots &data) {
        std::lock_guard lock(rec->mutex);
        if (rec->writer)
            rec->writer->write(std::uint32_t(rec->clock.elapsed()), universe, data);
    });
    recorder_ = std::move(rec);
    record_path_ = base + ".svdmx";
    record_error_.clear();
    record_video_ = media_.active();
    if (record_video_)
        record_error_ = media_.start_recording(base + ".mp4", lossless);
    // shortcut: feeds are started together, not frame-locked; sync drift of a frame or two is expected.
    for (const json &out : document_.data().value("outputs", json::array()))
        if (const auto it = feeds_.find(out.value("uuid", std::string()));
            it != feeds_.end() && it->second.first->active()) {
            record_video_ = true;
            const QString file = base + "." + feed_file_part(QString::fromStdString(out.value("name", ""))) + ".mp4";
            if (const QString error = it->second.first->start_recording(file, lossless); !error.isEmpty())
                record_error_ = error;
        }
    return {};
}

void Session::stopRecording() {
    if (!recorder_)
        return;
    dmx_.set_tap({});
    {
        std::lock_guard lock(recorder_->mutex);
        recorder_->writer->close();
        if (recorder_->writer->error())
            record_error_ = tr("Writing %1 failed").arg(record_path_);
        recorder_->writer.reset();
    }
    recorder_.reset();
    media_.stop_recording();
    for (const auto &[uuid, feed] : feeds_)
        feed.first->stop_recording();
}

QString Session::startPlayback(const QUrl &url) {
    if (recorder_)
        return tr("Stop recording before playing back");
    stopPlayback();
    const QString path = url.toLocalFile();
    try {
        playback_ = sv::DmxRecording::load(std::filesystem::path(path.toStdU16String()));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    playback_path_ = path;
    const QString base = recording_base(path);
    playback_sources_.clear();
    if (QFileInfo::exists(base + ".mp4")) {
        media_.open("video", base + ".mp4");
        playback_sources_.push_back(&media_);
    }
    for (const json &out : document_.data().value("outputs", json::array()))
        if (const QString video = base + "." + feed_file_part(QString::fromStdString(out.value("name", ""))) + ".mp4";
            out.value("kind", "") != "lumiverse" && QFileInfo::exists(video)) {
            sv::media::MediaSource *source = playback_feeds_[out.value("uuid", std::string())] = pooled_source();
            source->open("video", video); // beside the live feed, which keeps running for Live scopes
            playback_sources_.push_back(source);
        }
    for (sv::media::MediaSource *source : playback_sources_)
        source->set_playback(true, true, 1.0);
    playback_video_ = !playback_sources_.empty();
    update_ndi_out();
    playback_paused_ = false;
    playback_start_ms_ = 0;
    chase_t_ = 0, chase_running_ = !chase_;
    if (chase_)
        for (sv::media::MediaSource *source : playback_sources_)
            source->set_playback(false, false, 1.0); // starts with the timecode
    playback_clock_.start();
    playback_timer_.start();
    playback_tick();
    return {};
}

void Session::stopPlayback() {
    if (!playback_)
        return;
    playback_timer_.stop();
    for (const auto &[universe, _] : playback_sent_)
        dmx_.set_playback(universe, std::nullopt);
    playback_sent_.clear();
    playback_.reset();
    playback_path_.clear();
    if (playback_video_) { // back to the project's own sources
        playback_video_ = false;
        playback_sources_.clear();
        for (const auto &[uuid, source] : playback_feeds_)
            source->open({}, {}), idle_feeds_.push_back(source);
        playback_feeds_.clear();
        media_opened_ = json();
        update_media();
        update_ndi_out();
    }
}

void Session::pausePlayback(bool paused) {
    if (!playback_ || paused == playback_paused_)
        return;
    if (playback_video_)
        for (sv::media::MediaSource *source : playback_sources_)
            source->set_playback(!paused, true, 1.0);
    else if (paused)
        playback_start_ms_ = playback_time();
    else
        playback_clock_.restart();
    playback_paused_ = paused;
}

void Session::seekPlayback(double ms) {
    if (!playback_)
        return;
    if (playback_video_) {
        for (sv::media::MediaSource *source : playback_sources_)
            source->seek(qint64(ms));
    } else {
        playback_start_ms_ = qint64(ms);
        playback_clock_.restart();
    }
    playback_tick();
}

QString Session::setPlaybackChase(bool on, const QString &offset) {
    const QStringList parts = offset.trimmed().split(u':');
    std::array<int, 4> f{};
    bool ok = parts.size() == 3 || parts.size() == 4;
    for (int i = 0; ok && i < parts.size(); ++i)
        f[std::size_t(i)] = parts[i].toInt(&ok);
    if (!ok && !offset.trimmed().isEmpty())
        return tr("Timecode offset is HH:MM:SS:FF");
    const qint64 fps = qRound(dmx_.timecode().fps); // frames of the offset count at the incoming rate
    if (ok) // empty keeps the offset
        chase_offset_ms_ = ((f[0] * 60ll + f[1]) * 60 + f[2]) * 1000 + f[3] * 1000 / std::max<qint64>(fps, 1);
    if (on == chase_)
        return {};
    const qint64 t = playback_time();
    chase_ = on;
    if (playback_) {
        if (on) {
            chase_t_ = t, chase_running_ = true;
        } else { // carry on from where the timecode left the take
            playback_start_ms_ = chase_t_;
            playback_clock_.restart();
            for (sv::media::MediaSource *source : playback_sources_)
                source->set_playback(!playback_paused_, true, 1.0);
        }
    }
    Q_EMIT dmxChanged();
    return {};
}

void Session::chase_timecode(qint64 tc_ms) {
    if (!playback_ || !chase_)
        return;
    const bool running = tc_ms >= 0;
    if (running)
        chase_t_ = std::clamp<qint64>(tc_ms - chase_offset_ms_, 0, qint64(playback_->duration_ms()));
    if (playback_video_) {
        if (running != chase_running_)
            for (sv::media::MediaSource *source : playback_sources_)
                source->set_playback(running, false, 1.0);
        if (running && std::abs(playback_sources_.front()->position_ms() - chase_t_) > 100)
            for (sv::media::MediaSource *source : playback_sources_)
                source->seek(chase_t_);
    }
    chase_running_ = running;
}

void Session::setPlaybackReplace(bool replace) { setPlaybackScope("default", replace ? "recording" : "both"); }

void Session::setPlaybackScope(const QString &key, const QString &mode) {
    if (mode != "live" && mode != "recording" && mode != "both" && (!mode.isEmpty() || key == "default"))
        return;
    if (mode.isEmpty())
        playback_scopes_.erase(key.toStdString());
    else
        playback_scopes_[key.toStdString()] = mode.toStdString();
    playback_mix_dirty_ = true;
    resend_playback();
    update_ndi_out();
    Q_EMIT dmxChanged();
}

void Session::setPlaybackScopeFade(const QString &key, double fade) {
    if (fade < 0 || std::isnan(fade))
        playback_scope_fades_.erase(key.toStdString());
    else
        playback_scope_fades_[key.toStdString()] = std::min(fade, 1.0);
    playback_mix_dirty_ = true;
    resend_playback();
    update_ndi_out();
    Q_EMIT dmxChanged();
}

void Session::setPlaybackFade(double fade) {
    playback_fade_ = std::clamp(fade, 0.0, 1.0);
    resend_playback();
    update_ndi_out();
    Q_EMIT dmxChanged();
}

const std::map<std::uint16_t, sv::proto::PlaybackMix> &Session::playback_mix() {
    if (!playback_mix_dirty_)
        return playback_mix_;
    playback_mix_dirty_ = false;
    playback_mix_.clear();
    const json &project = document_.data();
    const json &objects = project.at("objects");
    const auto text = [](const json &o, const char *key) {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto mode_of = [&](const std::string &key) { // -1 = no override
        const auto it = playback_scopes_.find(key);
        return it == playback_scopes_.end() ? -1 : it->second == "live" ? 0 : it->second == "both" ? 2 : 1;
    };
    const auto fade_of = [&](const std::string &key) { // -1 = no own fader
        const auto it = playback_scope_fades_.find(key);
        return it == playback_scope_fades_.end() ? -1.0 : it->second;
    };
    struct Scoped {
        std::array<std::uint8_t, 512> mode;
        std::array<double, 512> fade;
    };
    std::map<std::uint16_t, Scoped> scoped;
    const auto universe = [&](int u) -> Scoped & {
        auto [it, added] = scoped.try_emplace(std::uint16_t(u));
        if (added) {
            it->second.mode.fill(std::uint8_t(mode_of("default"))), it->second.fade.fill(scope_fade("default"));
            playback_mix_[std::uint16_t(u)].snap.set(); // unpatched: switch
        }
        return it->second;
    };
    // Slot kinds: continuous GDTF attributes and strip pixels fade, everything else switches.
    for (const json &o : objects) {
        if (o.value("type", std::string()) == "pixelStrip") {
            for_each_channel(library_, o, [&](int u, int c) {
                universe(u);
                playback_mix_[std::uint16_t(u)].snap.reset(std::size_t(c - 1));
            });
            continue;
        }
        const json p = o.value("patch", json());
        const auto type = library_.find(o.value("gdtf", std::string()));
        const sv::gdtf::Mode *mode =
            type == library_.end() || !p.is_object() ? nullptr : type->second.mode(o.value("mode", std::string()));
        if (!mode)
            continue;
        const int u = p.value("universe", 1), a = p.value("address", 1);
        universe(u);
        sv::proto::PlaybackMix &mix = playback_mix_[std::uint16_t(u)];
        for (const sv::gdtf::Channel &ch : mode->channels) {
            if (!sv::gdtf::continuous_attribute(ch.attribute()))
                continue;
            for (std::uint16_t off : ch.offsets)
                if (const int slot = a - 1 + off; slot >= 0 && slot < 512)
                    mix.snap.reset(std::size_t(slot));
            if (const int fine = a - 1 + (ch.offsets.size() > 1 ? ch.offsets[1] : 0);
                ch.offsets.size() > 1 && ch.offsets[1] == ch.offsets[0] + 1 && fine > 0 && fine < 512)
                mix.fine.set(std::size_t(fine));
        }
    }
    // Mode and fader resolve independently: a part may set either.
    const auto apply = [&](const json &o, int m, double f) {
        for_each_channel(library_, o, [&](int u, int c) {
            Scoped &s = universe(u);
            if (m >= 0)
                s.mode[std::size_t(c - 1)] = std::uint8_t(m);
            if (f >= 0)
                s.fade[std::size_t(c - 1)] = f;
        });
    };
    // Outputs, then groups, then fixtures: the narrower scope wins.
    for (const json &out : project.value("outputs", json::array())) {
        const std::string uuid = text(out, "uuid");
        const int m = mode_of(uuid);
        const double f = fade_of(uuid);
        if (m < 0 && f < 0)
            continue;
        if (const int u = out.value("universe", 0); out.value("kind", std::string()) == "lumiverse" && u >= 1) {
            Scoped &s = universe(u);
            if (m >= 0)
                s.mode.fill(std::uint8_t(m));
            if (f >= 0)
                s.fade.fill(f);
        }
        for (const json &o : objects)
            if (text(o, "output") == uuid)
                apply(o, m, f);
    }
    for (const QVariant &g : control_groups()) {
        const QVariantMap group = g.toMap();
        const std::string key = group["key"].toString().toStdString();
        if (const int m = mode_of(key); m >= 0 || fade_of(key) >= 0)
            for (const QString &uuid : group["uuids"].toStringList())
                if (const int i = index_of(uuid.toStdString()); i >= 0)
                    apply(objects[std::size_t(i)], m, fade_of(key));
    }
    for (const json &o : objects)
        if (const std::string uuid = text(o, "uuid"); mode_of(uuid) >= 0 || fade_of(uuid) >= 0)
            apply(o, mode_of(uuid), fade_of(uuid));
    for (const auto &[u, s] : scoped) {
        sv::proto::PlaybackMix &mix = playback_mix_[u];
        for (std::size_t i = 0; i < 512; ++i)
            mix.weight[i] = s.mode[i] == 0 ? 0 : std::uint8_t(std::lround(255 * s.fade[i])),
            mix.htp[i] = s.mode[i] == 2;
    }
    return playback_mix_;
}

void Session::send_playback(std::uint16_t universe, const sv::proto::Slots &data) {
    const auto &mixes = playback_mix();
    sv::proto::PlaybackMix mix;
    if (const auto it = mixes.find(universe); it != mixes.end()) {
        mix = it->second;
    } else { // nothing patched there: the default scope, switching
        const std::string &m = playback_scopes_.at("default");
        mix.weight.fill(m == "live" ? 0 : std::uint8_t(std::lround(255 * scope_fade("default")))), mix.snap.set();
        if (m == "both")
            mix.htp.set();
    }
    mix.frame = data;
    for (std::uint8_t &w : mix.weight)
        w = std::uint8_t(std::lround(w * playback_fade_));
    dmx_.set_playback(universe, mix);
}

void Session::resend_playback() {
    if (playback_)
        for (const auto &[universe, data] : playback_sent_)
            send_playback(universe, data);
}

qint64 Session::playback_time() const {
    if (!playback_)
        return 0;
    if (chase_)
        return chase_t_;
    if (playback_video_)
        return playback_sources_.front()->position_ms(); // the player loops itself
    return playback_paused_ ? playback_start_ms_ : playback_start_ms_ + playback_clock_.elapsed();
}

void Session::playback_tick() {
    if (!playback_)
        return;
    // shortcut: the clock follows the video position at 44 Hz, so DMX lags the picture by up to one tick; drive it
    // from the video sink's frame times if that shows.
    chase_timecode(dmx_.timecode().ms);
    qint64 t = playback_time();
    if (!chase_ && !playback_video_ && t > qint64(playback_->duration_ms())) { // loop
        playback_start_ms_ = t = 0;
        playback_clock_.restart();
    }
    try {
        for (const auto &[universe, data] : playback_->state_at(std::uint32_t(std::max<qint64>(t, 0)))) {
            auto [it, added] = playback_sent_.try_emplace(universe, data);
            if (!added && it->second == data)
                continue;
            it->second = data;
            send_playback(universe, data);
        }
    } catch (const std::exception &e) {
        record_error_ = QString::fromUtf8(e.what());
        stopPlayback();
    }
}

QString Session::setMediaFile(const QString &kind, const QUrl &url) {
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile())
        return tr("Not a file: %1").arg(url.toLocalFile());
    std::string file = info.absoluteFilePath().toStdString();
    // Copied into the package like OBJ geometry; big videos stay linked so saves stay quick.
    if (info.size() <= 200ll * 1024 * 1024) {
        QString error;
        const QByteArray bytes = read_file(url, error);
        if (!error.isEmpty())
            return error;
        if (kind == "still" && QImage::fromData(bytes).isNull())
            return tr("Not an image: %1").arg(info.fileName());
        file = store_media(info, bytes);
    }
    json source = {{"kind", kind.toStdString()}, {"file", file}, {"name", info.fileName().toStdString()}};
    if (kind == "video")
        source.update({{"playing", true}, {"loop", true}, {"rate", 1.0}});
    if (apply(json::array({{{"op", "add"}, {"path", "/composition/source"}, {"value", source}}}),
              kind == "video" ? "Video source" : "Still image"))
        refresh();
    return {};
}

QString Session::importObj(const QUrl &url, const QString &unit, bool zUp, bool split) {
    QString error;
    const QByteArray bytes = read_file(url, error);
    if (!error.isEmpty())
        return error;
    std::vector<sv::ObjGroup> groups;
    try {
        groups = sv::parse_obj({bytes.constData(), std::size_t(bytes.size())});
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    const QFileInfo info(url.toLocalFile());
    const std::string entry = store_geometry(info, bytes);
    // shortcut: an MTL is stored under its own name, so two OBJs naming different files "x.mtl" share the last one;
    // rename on import when that shows up.
    for (const std::string &lib : sv::obj_mtllibs({bytes.constData(), std::size_t(bytes.size())})) {
        QFile mtl(info.dir().filePath(QString::fromStdString(lib)));
        if (mtl.open(QIODevice::ReadOnly)) {
            const QByteArray text = mtl.readAll();
            other_files_["geometry/" + lib] = {text.begin(), text.end()};
        }
    }
    rebuild_meshes();

    const double scale = sv::unit_scale(unit.toStdString());
    json patch = json::array();
    std::set<std::string> added;
    const auto piece = [&](const std::string &name, const std::string &group) {
        json o = sv::new_set_piece(name, entry);
        if (!group.empty())
            o["group"] = group;
        o["scale"] = {scale, scale, scale};
        if (zUp)
            o["rotation"] = {-90.0, 0.0, 0.0};
        added.insert(o["uuid"].get<std::string>());
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
    };
    if (split && groups.size() > 1)
        for (const sv::ObjGroup &g : groups)
            piece(g.name, g.name);
    else
        piece(info.completeBaseName().toStdString(), {});
    if (apply(patch, "Import OBJ")) {
        selection_ = added;
        refresh();
    }
    return {};
}

std::string Session::store_geometry(const QFileInfo &info, const QByteArray &bytes) {
    const std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
    std::string entry = "geometry/" + info.fileName().toStdString();
    for (int n = 2; other_files_.contains(entry) && other_files_[entry] != data; ++n)
        entry = "geometry/" + info.completeBaseName().toStdString() + "-" + std::to_string(n) + "." +
                info.suffix().toStdString();
    other_files_[entry] = data;
    return entry;
}

QString Session::importFbx(const QUrl &url) {
    QString error;
    const QByteArray bytes = read_file(url, error);
    if (!error.isEmpty())
        return error;
    sv::FbxScene scene;
    try {
        scene = sv::load_fbx({reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())});
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    const std::string entry = store_geometry(QFileInfo(url.toLocalFile()), bytes);
    rebuild_meshes();

    json patch = json::array();
    std::vector<std::string> uuids;
    std::set<std::string> added;
    for (const sv::FbxNode &n : scene.nodes) {
        json o;
        if (n.group >= 0) {
            o = sv::new_set_piece(n.name, entry);
            o["group"] = scene.groups[std::size_t(n.group)].name;
        } else {
            o = sv::new_set_piece(n.name, "");
            o["type"] = "group";
            o.erase("geometry");
        }
        o["position"] = n.transform.position;
        o["rotation"] = n.transform.rotation;
        o["scale"] = n.transform.scale;
        if (n.parent >= 0)
            o["parent"] = uuids[std::size_t(n.parent)];
        uuids.push_back(o["uuid"].get<std::string>());
        added.insert(uuids.back());
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
    }
    if (apply(patch, "Import FBX")) {
        selection_ = added;
        refresh();
    }
    return {};
}

QString Session::importMvr(const QUrl &url) {
    QString error;
    const QByteArray bytes = read_file(url, error);
    if (!error.isEmpty())
        return error;
    sv::MvrScene scene;
    try {
        resolume_warnings_.clear(); // shown on the Project screen as the last import's warnings
        scene = sv::mvr_import({reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())},
                               [this](const std::string &type, const std::string &mode) {
                                   return resolve_type(type, mode, resolume_warnings_);
                               });
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    for (const std::string &w : scene.warnings) {
        qCInfo(lcSession) << "MVR import:" << w.c_str();
        resolume_warnings_.push_back(QString::fromStdString(w));
    }
    for (auto &[entry, data] : scene.files)
        if (entry.starts_with("gdtf/")) {
            try {
                add_gdtf(entry.substr(5), std::move(data));
            } catch (const std::exception &e) {
                resolume_warnings_.push_back(tr("GDTF %1: %2").arg(QString::fromStdString(entry), e.what()));
            }
        } else if (!other_files_.contains(entry)) {
            other_files_[entry] = std::move(data);
        }
    rebuild_meshes();

    // Re-importing the same scene keeps its layers; objects that collide get fresh UUIDs, with parents following.
    const json &doc = document_.data();
    std::set<std::string> taken, layers;
    for (const json &o : doc.at("objects"))
        taken.insert(o.value("uuid", std::string()));
    for (const json &l : doc.value("layers", json::array()))
        layers.insert(l.value("uuid", std::string()));
    std::map<std::string, std::string> renamed;
    for (json &o : scene.objects)
        if (const std::string u = o.value("uuid", std::string()); taken.contains(u))
            o["uuid"] = renamed[u] = sv::new_uuid();
    json patch = json::array();
    if (!doc.contains("layers"))
        patch.push_back({{"op", "add"}, {"path", "/layers"}, {"value", json::array()}});
    for (const json &l : scene.layers)
        if (!layers.contains(l.value("uuid", std::string())))
            patch.push_back({{"op", "add"}, {"path", "/layers/-"}, {"value", l}});
    std::set<std::string> added;
    for (json &o : scene.objects) {
        if (const auto it = renamed.find(o.value("parent", json()).is_string() ? o["parent"].get<std::string>() : "");
            it != renamed.end())
            o["parent"] = it->second;
        added.insert(o["uuid"].get<std::string>());
        patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
    }
    if (!scene.aux_data.empty())
        patch.push_back({{"op", doc.contains("mvr") ? "replace" : "add"},
                         {"path", "/mvr"},
                         {"value", {{"auxData", scene.aux_data}}}});
    if (apply(patch, "Import MVR")) {
        selection_ = added;
        refresh();
    }
    return {};
}

QVariantMap Session::csvPreview(const QUrl &url) const {
    QString error;
    const QByteArray bytes = read_file(url, error);
    if (!error.isEmpty())
        return {{"error", error}};
    const auto rows = sv::parse_csv({bytes.constData(), std::size_t(bytes.size())});
    if (rows.empty())
        return {{"error", tr("The file is empty")}};
    QStringList header, fields;
    for (const std::string &h : rows[0])
        header.push_back(QString::fromStdString(h));
    QVariantList preview;
    for (std::size_t r = 1; r < rows.size() && r <= 8; ++r) {
        QStringList row;
        for (const std::string &c : rows[r])
            row.push_back(QString::fromStdString(c));
        preview.push_back(row);
    }
    QVariantMap mapping;
    for (const auto &[field, column] : sv::guess_csv_mapping(rows[0]))
        mapping[QString::fromStdString(field)] = column;
    for (const char *f : sv::csv_fields)
        fields.push_back(f);
    return {{"header", header},
            {"rows", preview},
            {"mapping", mapping},
            {"fields", fields},
            {"count", int(rows.size()) - 1}};
}

QString Session::importCsv(const QUrl &url, const QVariantMap &mapping, const QString &unit, bool zUp) {
    QString error;
    const QByteArray bytes = read_file(url, error);
    if (!error.isEmpty())
        return error;
    sv::CsvMapping m;
    for (auto it = mapping.begin(); it != mapping.end(); ++it)
        m[it.key().toStdString()] = it.value().toInt();
    sv::CsvImportOptions options;
    options.scale = sv::unit_scale(unit.toStdString());
    options.z_up = zUp;
    resolume_warnings_.clear();
    options.resolve = [this](const std::string &type, const std::string &mode) {
        return resolve_type(type, mode, resolume_warnings_);
    };
    try {
        const json patch =
            sv::csv_import(document_.data(), sv::parse_csv({bytes.constData(), std::size_t(bytes.size())}), m, options);
        if (patch.empty())
            return tr("No fixture rows found; check the column mapping");
        if (!apply(patch, "Import CSV"))
            return tr("The CSV could not be applied, see the log");
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}

void Session::rebuild_meshes() {
    auto lib = std::make_shared<sv::render::MeshLibrary>();
    for (const auto &[entry, bytes] : other_files_) {
        const QString name = QString::fromStdString(entry);
        const bool obj = name.endsWith(".obj", Qt::CaseInsensitive), fbx = name.endsWith(".fbx", Qt::CaseInsensitive);
        if (!entry.starts_with("geometry/") ||
            !(obj || fbx || name.endsWith(".glb", Qt::CaseInsensitive) || name.endsWith(".gltf", Qt::CaseInsensitive)))
            continue;
        try {
            std::vector<sv::ObjGroup> groups;
            if (obj) {
                const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                sv::ObjMaterials materials;
                for (const std::string &mtl : sv::obj_mtllibs(text))
                    if (const auto it = other_files_.find("geometry/" + mtl); it != other_files_.end())
                        materials.merge(
                            sv::parse_mtl({reinterpret_cast<const char *>(it->second.data()), it->second.size()}));
                groups = sv::parse_obj(text, materials);
            } else
                groups = fbx ? sv::load_fbx(bytes).groups : sv::parse_gltf(bytes);
            // FBX groups are in node space and only make sense per node, so FBX has no whole-file mesh.
            std::vector<float> all;
            for (sv::ObjGroup &g : groups) {
                if (!fbx)
                    all.insert(all.end(), g.vertices.begin(), g.vertices.end());
                const QVector4D colour(g.colour[0], g.colour[1], g.colour[2], g.colour[3]);
                (*lib)[entry + "#" + g.name] = sv::render::make_mesh(std::move(g.vertices), colour);
            }
            if (!fbx && !groups.empty()) {
                const auto same = [&](const sv::ObjGroup &g) { return g.colour == groups.front().colour; };
                const auto &c = groups.front().colour;
                (*lib)[entry] = sv::render::make_mesh(std::move(all), std::ranges::all_of(groups, same)
                                                                          ? QVector4D(c[0], c[1], c[2], c[3])
                                                                          : QVector4D());
            }
        } catch (const std::exception &e) {
            qCWarning(lcSession) << "geometry" << entry.c_str() << "failed:" << e.what();
        }
    }
    meshes_ = std::move(lib);
}

std::filesystem::path Session::autosave_target() const {
    if (!path_.empty())
        return path_;
    return std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdU16String()) /
           "Untitled.svproj";
}

void Session::autosave() {
    if (!document_.dirty())
        return;
    const std::filesystem::path target = autosave_target();
    try {
        std::filesystem::create_directories(target.parent_path());
        sv::write_autosave(target, package());
    } catch (const std::exception &e) {
        qCWarning(lcSession) << "autosave failed:" << e.what();
    }
}

void Session::check_recovery() {
    const std::filesystem::path target = autosave_target();
    std::error_code ec;
    // An untitled autosave has no saved file to compare with, so any one left behind is newer.
    const bool newer =
        path_.empty() ? std::filesystem::exists(sv::autosave_path(target), ec) : sv::has_newer_autosave(target);
    recovery_ = newer ? QString::fromStdU16String(sv::autosave_path(target).u16string()) : QString();
}

QString Session::recover() {
    if (recovery_.isEmpty())
        return tr("Nothing to recover");
    try {
        sv::Package pkg = sv::load_package(recovery_.toStdU16String());
        load(std::move(pkg)); // keeps path_, so the next save replaces the real file
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    recovery_.clear(); // load() re-checked, but the user has chosen; the file goes on the next save
    document_.mark_unsaved();
    Q_EMIT changed();
    return {};
}

std::string Session::store_media(const QFileInfo &info, const QByteArray &bytes) {
    std::string file = "media/" + info.fileName().toStdString();
    for (int n = 2;
         other_files_.contains(file) && other_files_[file] != std::vector<std::uint8_t>(bytes.begin(), bytes.end());
         ++n)
        file = "media/" + info.completeBaseName().toStdString() + "-" + std::to_string(n) + "." +
               info.suffix().toStdString();
    other_files_[file] = {bytes.begin(), bytes.end()};
    return file;
}

namespace {
QSettings settings() { return QSettings("SceneVisualiser", "Scene Visualiser"); }

std::string media_file(const json &project) {
    const json &comp = project.value("composition", json::object());
    const json src = comp.is_object() ? comp.value("source", json::object()) : json::object();
    return src.is_object() ? src.value("file", std::string()) : std::string();
}
} // namespace

void Session::remember_recent(const std::filesystem::path &path) {
    const QString p = QString::fromStdU16String(path.u16string());
    QStringList list = settings().value("recentFiles").toStringList();
    list.removeAll(p);
    list.prepend(p);
    settings().setValue("recentFiles", list.mid(0, 10));
}

QVariantList Session::recent_files() const {
    QVariantList out;
    for (const QString &p : settings().value("recentFiles").toStringList())
        if (const QFileInfo info(p); info.isFile())
            out.append(QVariantMap{{"path", p}, {"name", info.completeBaseName()}, {"url", QUrl::fromLocalFile(p)}});
    return out;
}

QStringList Session::missing_files() const {
    std::set<QString> out;
    for (const json &o : document_.data().at("objects")) {
        if (const std::string type = o.value("gdtf", std::string()); !type.empty() && !library_.contains(type))
            out.insert(tr("Fixture type %1").arg(QString::fromStdString(type)));
        if (const std::string geo = o.value("geometry", std::string()); !geo.empty() && !other_files_.contains(geo))
            out.insert(tr("Geometry %1").arg(QString::fromStdString(geo)));
    }
    if (const std::string file = media_file(document_.data()); !file.empty()) {
        const bool found =
            file.starts_with("media/") ? other_files_.contains(file) : QFileInfo(QString::fromStdString(file)).isFile();
        if (!found)
            out.insert(tr("Media %1").arg(QString::fromStdString(file)));
    }
    return {out.begin(), out.end()};
}

QStringList Session::linked_media() const {
    const std::string file = media_file(document_.data());
    if (file.empty() || file.starts_with("media/") || !QFileInfo(QString::fromStdString(file)).isFile())
        return {};
    return {QString::fromStdString(file)};
}

QString Session::packMedia() {
    const QStringList linked = linked_media();
    if (linked.isEmpty())
        return tr("No linked media to pack");
    QString error;
    const QByteArray bytes = read_file(QUrl::fromLocalFile(linked.first()), error);
    if (!error.isEmpty())
        return error;
    const std::string file = store_media(QFileInfo(linked.first()), bytes);
    if (apply(json::array({{{"op", "replace"}, {"path", "/composition/source/file"}, {"value", file}}}), "Pack media"))
        refresh();
    return {};
}

void Session::discardChanges() {
    std::error_code ec;
    std::filesystem::remove(sv::autosave_path(autosave_target()), ec);
}

void Session::discardRecovery() {
    std::error_code ec;
    std::filesystem::remove(recovery_.toStdU16String(), ec);
    recovery_.clear();
    Q_EMIT changed();
}

namespace {
json drawing_of(const json &project, const std::string &kind, int *index = nullptr) {
    if (index)
        *index = -1;
    if (project.contains("drawings") && project["drawings"].is_array())
        for (std::size_t i = 0; i < project["drawings"].size(); ++i) {
            const json &d = project["drawings"][i];
            if (d.is_object() && d.value("kind", std::string()) == kind) {
                if (index)
                    *index = int(i);
                return d;
            }
        }
    return {{"uuid", sv::new_uuid()},
            {"kind", kind},
            {"sheet", "A3"},
            {"scale", 0},
            {"title", std::string()},
            {"hiddenLayers", json::array()},
            {"dimensions", json::array()}};
}
} // namespace

QVariantMap Session::drawing(const QString &kind) const {
    return to_variant(drawing_of(document_.data(), kind.toStdString())).toMap();
}

void Session::setDrawing(const QString &kind, const QString &key, const QVariant &value) {
    int index = -1;
    json d = drawing_of(document_.data(), kind.toStdString(), &index);
    json v = from_variant(value);
    if (d.contains(key.toStdString()) && d[key.toStdString()].is_number_integer() && v.is_number())
        v = std::llround(v.get<double>());
    if (d.value(key.toStdString(), json()) == v)
        return;
    d[key.toStdString()] = std::move(v);
    const bool list = document_.data().contains("drawings") && document_.data()["drawings"].is_array();
    if (!list)
        apply(json::array({{{"op", "add"}, {"path", "/drawings"}, {"value", json::array({d})}}}), "Edit drawing");
    else
        apply(json::array({{{"op", index < 0 ? "add" : "replace"},
                            {"path", "/drawings/" + (index < 0 ? std::string("-") : std::to_string(index))},
                            {"value", d}}}),
              "Edit drawing");
}

sv::render::DrawingSheet Session::drawing_sheet(const QString &kind) const {
    sv::render::DrawingSheet sheet = sv::render::sheet_from_json(drawing_of(document_.data(), kind.toStdString()));
    sheet.project_name = name_.toStdString();
    return sheet;
}

QString Session::exportDrawing(const QString &kind, const QUrl &url) {
    const sv::render::DrawingSheet sheet = drawing_sheet(kind);
    const QSizeF mm = sv::render::sheet_size_mm(sheet);
    const QString path = url.toLocalFile();
    QPainter p;
    std::unique_ptr<QPaintDevice> device;
    if (path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        auto svg = std::make_unique<QSvgGenerator>();
        svg->setFileName(path);
        svg->setSize(QSize(int(mm.width() * 10), int(mm.height() * 10))); // tenths of a mm at 254 dpi
        svg->setResolution(254);
        svg->setViewBox(QRectF(QPointF(), mm)); // user units are page millimetres
        svg->setTitle(name_);
        device = std::move(svg);
        if (!p.begin(device.get()))
            return tr("Cannot write %1").arg(path);
    } else {
        auto pdf = std::make_unique<QPdfWriter>(path);
        pdf->setPageSize(QPageSize(sheet.paper == "A4" ? QPageSize::A4 : QPageSize::A3));
        pdf->setPageOrientation(QPageLayout::Landscape);
        pdf->setPageMargins({}, QPageLayout::Millimeter);
        pdf->setResolution(1200);
        pdf->setTitle(name_);
        device = std::move(pdf);
        if (!p.begin(device.get()))
            return tr("Cannot write %1").arg(path);
        p.scale(1200 / 25.4, 1200 / 25.4); // page millimetres
    }
    sv::render::paint_drawing(p, document_.data(), library_, *meshes_, sheet);
    return p.end() ? QString() : tr("Cannot write %1").arg(path);
}

QVariantList Session::list_of(const char *key) const {
    const json &p = document_.data();
    return p.contains(key) && p[key].is_array() ? to_variant(p[key]).toList() : QVariantList();
}

QVariant Session::preference(const QString &key, const QVariant &fallback) {
    if (key == "autoUpdate")
        return sv::updater::automatic();
    return settings().value(key, fallback);
}

void Session::setPreference(const QString &key, const QVariant &value) {
    if (key == "autoUpdate")
        return sv::updater::set_automatic(value.toBool());
    settings().setValue(key, value);
    if (key == "controls" && instance())
        Q_EMIT instance()->changed();
    if (key == "crashReports")
        sv::crash::set_consent(value.toBool());
    if (key.startsWith("output") && instance())
        instance()->apply_output();
}

void Session::apply_output() {
    sv::proto::OutputConfig c;
    c.enabled = preference("outputEnabled", false).toBool();
    c.sacn = preference("outputProtocol", "artnet").toString() == "sacn";
    c.target = preference("outputTarget", "").toString().trimmed().toStdString();
    c.universe_offset = preference("outputUniverseOffset", 0).toInt();
    c.priority = std::uint8_t(std::clamp(preference("outputPriority", 100).toInt(), 0, 200));
    dmx_.set_output(c);
    update_ndi_out();
}

QVariantMap Session::output_status() const {
    const sv::proto::OutputStatus st = dmx_.output_status();
    return {{"enabled", preference("outputEnabled", false).toBool()}, // the engine's own status lags by a post
            {"protocol", preference("outputProtocol", "artnet").toString()},
            {"target", preference("outputTarget", "").toString()},
            {"offset", preference("outputUniverseOffset", 0).toInt()},
            {"rate", st.packets_per_s},
            {"universes", int(st.universes)},
            {"error", QString::fromStdString(st.error)},
            {"ddpTargets", int(st.ddp_targets)},
            {"ddpRate", st.ddp_packets_per_s},
            {"ddpError", QString::fromStdString(st.ddp_error)},
            {"ndi", [this] {
                 QVariantList streams;
                 for (const auto &[uuid, sender] : ndi_out_)
                     streams.push_back(QVariantMap{{"name", sender.first}, {"fps", sender.second->fps()}});
                 return streams;
             }()}};
}

void Session::update_ddp() {
    std::vector<sv::proto::DdpTarget> targets;
    std::vector<PixelOutput::Target> pixels;
    for (const json &o : document_.data().value("objects", json::array())) {
        const std::string type = o.value("type", std::string());
        const json ddp = o.value("ddp", json::object());
        if ((type != "pixelStrip" && type != "ledWall") || !ddp.is_object() || !ddp.value("enabled", false) ||
            ddp.value("target", std::string()).empty())
            continue;
        sv::proto::DdpTarget t{.key = o.value("uuid", std::string()),
                               .ip = ddp.value("target", std::string()),
                               .port = std::uint16_t(std::clamp(ddp.value("port", int(sv::proto::ddp_port)), 1, 65535)),
                               .offset = std::uint32_t(std::max(0, ddp.value("offset", 0)))};
        const json slice = o.value("slice", json::object());
        if (type == "ledWall") {
            const auto derived = sv::wall_derived(o);
            const bool strings = o.contains("strings") && o.at("strings").is_array() && !o.at("strings").empty();
            pixels.push_back({t.key, slice, strings ? int(o.at("strings").size()) : derived.pixels_x, derived.pixels_y,
                              strings});
        } else if (o.value("pixelSource", std::string("dmx")) == "slice") {
            pixels.push_back({t.key, slice, std::max(1, o.value("pixels", 1)), std::max(1, o.value("rows", 1))});
        } else if (o.value("patch", json()).is_object()) {
            // DDP controllers take RGB(W) and apply their own LED order, so Arena's colour format is undone here.
            std::string format = o.value("colorFormat", std::string("rgb"));
            std::ranges::transform(format, format.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            t.rgbw = format.find('w') != std::string::npos;
            const std::string order = t.rgbw ? "rgbw" : "rgb";
            const int n = std::max(0, o.value("pixels", 0)) * std::max(1, o.value("rows", 1));
            for (int i = 0; i < n; ++i) {
                const sv::DmxAddress at = sv::strip_pixel_address(o, i);
                for (char c : order) {
                    const auto k = format.find(c);
                    const int slot = k == std::string::npos ? 0 : at.address + int(k);
                    t.channels.emplace_back(std::uint16_t(at.universe), std::uint16_t(slot <= 512 ? slot : 0));
                }
            }
        } else {
            continue; // an unpatched DMX strip has nothing to send
        }
        targets.push_back(std::move(t));
    }
    if (targets != ddp_targets_)
        dmx_.set_ddp(ddp_targets_ = std::move(targets));
    if (pixels != pixel_targets_ || !pixels.empty()) {
        pixel_targets_ = std::move(pixels);
        push_pixel_inputs();
    }
}

void Session::push_pixel_inputs() {
    pixel_output_.set({snapshot_, composition_, feed_sources(), playback_feed_sources(), playback_opacity(),
                       pixel_targets_, 44});
}

void Session::checkForUpdates() { sv::updater::check_now(); }

QVariantMap Session::app_info() {
    return {{"version", QCoreApplication::applicationVersion()},
            {"updates", sv::updater::supported()},
            {"crashUploads", sv::crash::uploads_supported()},
            {"crashFolder", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/crashes"}};
}

// Light control (programmer)

Session::EncodedFixture Session::encode_fixture(const std::string &uuid, const json &attributes, int head,
                                                bool open_shutter) const {
    const int i = index_of(uuid);
    const json *o = i < 0 ? nullptr : &document_.data().at("objects")[i];
    const auto type = o ? library_.find(o->value("gdtf", std::string())) : library_.end();
    if (type == library_.end() || type->second.modes.empty())
        throw std::runtime_error("No fixture with a known type and mode: " + uuid);
    const sv::gdtf::Mode *mode = type->second.mode(o->value("mode", std::string()));
    if (!mode)
        mode = &type->second.modes.front();
    const json patch = o->value("patch", json());
    if (!patch.is_object())
        throw std::runtime_error("Fixture is not patched: " + uuid);
    EncodedFixture out{std::uint16_t(patch.value("universe", 1)), patch.value("address", 1), {}, {}};
    for (const auto &[attr, value] : attributes.items())
        open_shutter = open_shutter && !attr.starts_with("Shutter");
    if (open_shutter)
        out.bytes = sv::gdtf::open_shutter(*mode);
    for (const auto &[attr, value] : attributes.items()) {
        const auto b = sv::gdtf::encode(*mode, attr, value.get<double>(), head);
        if (b.empty())
            out.unknown.push_back(attr);
        out.bytes.insert(out.bytes.end(), b.begin(), b.end());
    }
    return out;
}

void Session::push_programmer() {
    std::map<std::uint16_t, std::pair<sv::proto::Slots, std::bitset<512>>> held;
    std::erase_if(programmer_, [&](const auto &p) { return index_of(p.first) < 0 || p.second.empty(); });
    const auto hold = [&](const EncodedFixture &e) {
        auto &[slots, mask] = held[e.universe];
        for (const auto &b : e.bytes)
            if (const int ch = e.address - 1 + b.offset; ch >= 0 && ch < 512)
                slots[ch] = b.value, mask.set(ch);
    };
    for (const auto &[uuid, attributes] : programmer_) {
        try {
            json whole = attributes;
            whole.erase("heads");
            hold(encode_fixture(uuid, whole, -1, true));
            if (attributes.contains("heads")) // after the whole fixture, so a head's own value wins
                for (const auto &[head, values] : attributes["heads"].items())
                    hold(encode_fixture(uuid, values, std::stoi(head), false));
        } catch (const std::exception &) {
            // unpatched or untyped since it was set: nothing to hold
        }
    }
    for (std::uint16_t u : programmer_universes_)
        if (!held.contains(u))
            dmx_.set_programmer(u, std::nullopt);
    programmer_universes_.clear();
    for (const auto &[u, p] : held)
        dmx_.set_programmer(u, p), programmer_universes_.insert(u);
}

void Session::setLight(const QStringList &uuids, const QVariantMap &attributes, int head) {
    for (const QString &u : uuids) {
        const int i = index_of(u.toStdString());
        if (i < 0 || document_.data().at("objects")[i].value("type", std::string()) != "fixture")
            continue;
        json &whole = programmer_[u.toStdString()];
        const std::string key = std::to_string(head);
        for (auto it = attributes.begin(); it != attributes.end(); ++it) {
            const std::string attr = it.key().toStdString();
            bool on_head = head >= 0;
            if (on_head)
                try {
                    on_head = !encode_fixture(u.toStdString(), {{attr, 0.0}}, head, false).bytes.empty();
                } catch (const std::exception &) {
                    on_head = false;
                }
            json &held = on_head ? whole["heads"][key] : whole;
            if (it.value().isNull())
                held.erase(attr);
            else
                held[attr] = it.value().toDouble();
        }
        if (whole.contains("heads")) {
            json &heads = whole["heads"];
            for (auto h = heads.begin(); h != heads.end();)
                h = h->empty() ? heads.erase(h) : std::next(h);
            if (heads.empty())
                whole.erase("heads");
        }
    }
    push_programmer();
    Q_EMIT changed();
}

void Session::releaseLights(const QStringList &uuids) {
    if (uuids.isEmpty())
        programmer_.clear(), located_.clear();
    for (const QString &u : uuids)
        programmer_.erase(u.toStdString()), located_.erase(u.toStdString());
    push_programmer();
    Q_EMIT changed();
}

void Session::locate(const QStringList &uuids, bool on) {
    static const std::map<std::string, double> look{{"Dimmer", 1},     {"ColorAdd_R", 1}, {"ColorAdd_G", 1},
                                                    {"ColorAdd_B", 1}, {"ColorAdd_W", 1}, {"ColorSub_C", 0},
                                                    {"ColorSub_M", 0}, {"ColorSub_Y", 0}};
    for (const QString &q : uuids) {
        const std::string u = q.toStdString();
        const int i = index_of(u);
        if (i < 0 || document_.data().at("objects")[i].value("type", std::string()) != "fixture" ||
            located_.contains(u) == on)
            continue;
        json &held = programmer_[u];
        if (on) {
            json before = json::object();
            for (const auto &[attr, value] : look) {
                if (held.contains(attr))
                    before[attr] = held[attr];
                held[attr] = value;
            }
            located_[u] = before;
        } else {
            for (const auto &[attr, value] : look)
                held.erase(attr);
            held.update(located_[u]);
            located_.erase(u);
        }
    }
    push_programmer();
    Q_EMIT changed();
}

QVariantList Session::looks() const {
    QVariantList out;
    if (document_.data().contains("looks") && document_.data()["looks"].is_array())
        for (const json &l : document_.data()["looks"])
            out << QVariantMap{{"uuid", QString::fromStdString(l.value("uuid", std::string()))},
                               {"name", QString::fromStdString(l.value("name", std::string()))}};
    return out;
}

QString Session::saveLook(const QString &name) {
    if (name.trimmed().isEmpty())
        return tr("Give the look a name");
    json held = json::object();
    for (const auto &[uuid, attributes] : programmer_)
        held[uuid] = attributes;
    const json look{{"uuid", sv::new_uuid()}, {"name", name.trimmed().toStdString()}, {"programmer", held}};
    json patch = json::array();
    if (!document_.data().contains("looks"))
        patch.push_back({{"op", "add"}, {"path", "/looks"}, {"value", json::array()}});
    patch.push_back({{"op", "add"}, {"path", "/looks/-"}, {"value", look}});
    return QString::fromStdString(try_apply(patch, "Save look"));
}

void Session::recallLook(const QString &uuid) {
    if (!document_.data().contains("looks") || !document_.data()["looks"].is_array())
        return;
    for (const json &l : document_.data()["looks"]) {
        if (l.value("uuid", std::string()) != uuid.toStdString())
            continue;
        programmer_.clear(), located_.clear();
        if (l.contains("programmer") && l["programmer"].is_object())
            for (const auto &[fixture, attributes] : l["programmer"].items())
                if (attributes.is_object())
                    programmer_[fixture] = attributes; // push_programmer drops fixtures deleted since
        push_programmer();
        Q_EMIT changed();
        return;
    }
}

void Session::deleteLook(const QString &uuid) {
    const json looks = document_.data().value("looks", json::array());
    for (std::size_t i = 0; i < looks.size(); ++i)
        if (looks[i].value("uuid", std::string()) == uuid.toStdString()) {
            apply(json::array({{{"op", "remove"}, {"path", "/looks/" + std::to_string(i)}}}), "Delete look");
            return;
        }
}

QStringList Session::located() const {
    QStringList out;
    for (const auto &[uuid, before] : located_)
        out << QString::fromStdString(uuid);
    return out;
}

QVariantMap Session::programmer() const {
    QVariantMap out;
    for (const auto &[uuid, attributes] : programmer_)
        out[QString::fromStdString(uuid)] = to_variant(attributes);
    return out;
}

QStringList Session::selected_uuids() const {
    QStringList out;
    for (const std::string &u : selection_)
        out << QString::fromStdString(u);
    return out;
}

QStringList Session::controlAttributes(const QStringList &uuids) const {
    std::set<std::string> names;
    for (const QString &u : uuids) {
        const int i = index_of(u.toStdString());
        if (i < 0)
            continue;
        const json &o = document_.data().at("objects")[i];
        const auto type = library_.find(o.value("gdtf", std::string()));
        if (type == library_.end() || type->second.modes.empty())
            continue;
        const sv::gdtf::Mode *mode = type->second.mode(o.value("mode", std::string()));
        for (const auto &ch : (mode ? *mode : type->second.modes.front()).channels)
            for (const auto &fn : ch.functions)
                names.insert(fn.attribute);
    }
    QStringList out;
    for (const std::string &n : names)
        out << QString::fromStdString(n);
    return out;
}

int Session::controlHeads(const QStringList &uuids) const {
    int fewest = -1;
    for (const QString &u : uuids) {
        const int i = index_of(u.toStdString());
        if (i < 0)
            continue;
        const json &o = document_.data().at("objects")[i];
        const auto type = library_.find(o.value("gdtf", std::string()));
        if (type == library_.end() || type->second.modes.empty())
            continue;
        const sv::gdtf::Mode *mode = type->second.mode(o.value("mode", std::string()));
        const int n = int((mode ? *mode : type->second.modes.front()).heads.size());
        fewest = fewest < 0 ? n : std::min(fewest, n);
    }
    return std::max(fewest, 0);
}

QVariantList Session::control_groups() const {
    const json &project = document_.data();
    const json &objects = project.at("objects");
    std::map<std::string, const json *> by_uuid;
    for (const json &o : objects)
        by_uuid[o.value("uuid", std::string())] = &o;
    std::map<std::string, QStringList> rig, types;
    for (const json &o : objects) {
        if (o.value("type", std::string()) != "fixture")
            continue;
        const QString uuid = QString::fromStdString(o.value("uuid", std::string()));
        types[o.value("gdtf", std::string())] << uuid;
        const auto parent = [](const json &x) { // null when at the top
            const json p = x.value("parent", json());
            return p.is_string() ? p.get<std::string>() : std::string();
        };
        std::set<std::string> seen; // a parent cycle in a hand-edited file must not hang
        for (std::string p = parent(o); !p.empty() && seen.insert(p).second;) {
            const auto it = by_uuid.find(p);
            if (it == by_uuid.end())
                break;
            if (it->second->value("type", std::string()) == "group")
                rig[p] << uuid;
            p = parent(*it->second);
        }
    }
    QVariantList out;
    for (const json &g : project.value("groups", json::array())) {
        QStringList uuids;
        for (const json &u : g.value("fixtures", json::array()))
            if (u.is_string() && index_of(u.get<std::string>()) >= 0)
                uuids << QString::fromStdString(u.get<std::string>());
        out << QVariantMap{{"key", QString::fromStdString(g.value("uuid", std::string()))},
                           {"name", QString::fromStdString(g.value("name", std::string()))},
                           {"kind", "saved"},
                           {"uuids", uuids}};
    }
    for (const auto &[uuid, uuids] : rig) {
        const std::string name = by_uuid[uuid]->value("name", std::string());
        out << QVariantMap{{"key", QString::fromStdString(uuid)},
                           {"name", name.empty() ? tr("Unnamed group") : QString::fromStdString(name)},
                           {"kind", "rig"},
                           {"uuids", uuids}};
    }
    for (const auto &[gdtf, uuids] : types) {
        const auto type = library_.find(gdtf);
        const QString name = type != library_.end() && !type->second.name.empty()
                                 ? QString::fromStdString(type->second.name)
                                 : QFileInfo(QString::fromStdString(gdtf)).completeBaseName();
        out << QVariantMap{
            {"key", QString::fromStdString("type:" + gdtf)}, {"name", name}, {"kind", "type"}, {"uuids", uuids}};
    }
    return out;
}

QString Session::saveGroup(const QString &name, const QStringList &uuids) {
    if (name.trimmed().isEmpty())
        return tr("Give the group a name");
    json fixtures = json::array();
    for (const QString &u : uuids)
        fixtures.push_back(u.toStdString());
    const json group{{"uuid", sv::new_uuid()}, {"name", name.trimmed().toStdString()}, {"fixtures", fixtures}};
    json patch = json::array();
    if (!document_.data().contains("groups"))
        patch.push_back({{"op", "add"}, {"path", "/groups"}, {"value", json::array()}});
    patch.push_back({{"op", "add"}, {"path", "/groups/-"}, {"value", group}});
    return QString::fromStdString(try_apply(patch, "Save group"));
}

void Session::deleteGroup(const QString &uuid) {
    const json groups = document_.data().value("groups", json::array());
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (groups[i].value("uuid", std::string()) == uuid.toStdString()) {
            apply(json::array({{{"op", "remove"}, {"path", "/groups/" + std::to_string(i)}}}), "Delete group");
            return;
        }
}
