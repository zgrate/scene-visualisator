#include "session.h"

#include "crash_reports.h"
#include "updater.h"

#include "core/fbx.h"
#include "core/interchange.h"
#include "core/mvr.h"
#include "core/resolume.h"
#include "render/drawing_painter.h"
#include "render/test_pattern.h"

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
#include <ranges>

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
    for (const json &o : objects)
        if (const json &p = o.value("patch", json()); p.is_object()) {
            const int u = p.value("universe", 1), end = p.value("address", 1) + footprint(lib, o);
            if (u > universe || (u == universe && end > next))
                universe = u, next = end;
        }
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
        Q_EMIT dmxChanged();
    });
    status_timer_.start();
    autosave_timer_.setInterval(2 * 60 * 1000); // AT-6: at most 2 minutes of lost work
    connect(&autosave_timer_, &QTimer::timeout, this, &Session::autosave);
    autosave_timer_.start();
}

Session::~Session() {
    dmx_.stop();
    the_session = nullptr;
}

Session *Session::instance() { return the_session; }

Session *Session::create(QQmlEngine *, QJSEngine *) {
    QJSEngine::setObjectOwnership(the_session, QJSEngine::CppOwnership);
    return the_session;
}

void Session::set_project(sv::json project, QString name) {
    name_ = std::move(name);
    document_ = sv::Document(std::move(project));
    selection_.clear();
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
    ++revision_;
    Q_EMIT changed();
}

bool Session::apply(const json &patch, const std::string &label) {
    try {
        document_.apply(patch, label);
    } catch (const std::exception &e) {
        qCWarning(lcSession) << "edit" << label.c_str() << "rejected:" << e.what();
        return false;
    }
    refresh();
    return true;
}

int Session::index_of(const std::string &uuid) const {
    const json &objects = document_.data().at("objects");
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (objects[i].value("uuid", std::string()) == uuid)
            return int(i);
    return -1;
}

void Session::preview_move(const std::string &uuid, QVector3D position) {
    const int i = index_of(uuid);
    if (i < 0)
        return;
    moving_ = {uuid, position};
    auto copy = std::make_shared<json>(document_.data());
    (*copy)["objects"][i]["position"] = {position.x(), position.y(), position.z()};
    snapshot_ = std::move(copy);
    ++revision_; // no changed(): QML models stay put until the drop
}

void Session::commit_move() {
    if (!moving_)
        return;
    const auto [uuid, p] = *std::exchange(moving_, std::nullopt);
    const int i = index_of(uuid);
    if (i < 0 || !apply(json::array({{{"op", "replace"},
                                      {"path", "/objects/" + std::to_string(i) + "/position"},
                                      {"value", {p.x(), p.y(), p.z()}}}}),
                        "Move"))
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

void Session::add_object(json object, const char *label) {
    const std::string uuid = object["uuid"];
    if (apply(json::array({{{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(object)}}}), label))
        select(uuid, false);
}

void Session::addFixture(const QString &gdtf, const QString &mode) {
    const auto it = library_.find(gdtf.toStdString());
    if (it == library_.end())
        return;
    const json &objects = document_.data().at("objects");
    const int id = next_fixture_id(objects);
    json f = sv::new_fixture(it->second.name + " " + std::to_string(id), gdtf.toStdString(), mode.toStdString());
    f["fixtureId"] = id;
    f["position"] = {0.0, 4.0, 0.0};
    f["patch"] = next_patch(objects, library_, footprint(library_, f));
    add_object(std::move(f), "Add fixture");
}

void Session::addWall() {
    json w = sv::new_led_wall("LED wall");
    w["position"] = {0.0, 1.0, 0.0};
    add_object(std::move(w), "Add LED wall");
}

void Session::addStrip() {
    json s = sv::new_pixel_strip("Pixel strip");
    s["position"] = {0.0, 3.0, 0.0};
    s["patch"] = next_patch(document_.data().at("objects"), library_, std::min(512, footprint(library_, s)));
    add_object(std::move(s), "Add pixel strip");
}

void Session::addTruss() {
    json t = sv::new_truss("Truss", "straight", 6);
    t["position"] = {0.0, 6.0, 0.0};
    add_object(std::move(t), "Add truss");
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

void Session::newProject() {
    path_.clear();
    other_files_.clear();
    rebuild_meshes();
    set_project(sv::new_project());
}

void Session::load(sv::Package pkg) {
    other_files_.clear();
    for (auto &[entry, bytes] : pkg.files)
        if (entry.starts_with("gdtf/")) {
            try {
                add_gdtf(entry.substr(5), std::move(bytes));
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
    return {};
}

sv::Package Session::package() const {
    sv::Package pkg{document_.data(), other_files_};
    for (const json &o : document_.data().at("objects")) // only the types the rig uses travel with it
        if (const auto it = gdtf_files_.find(o.value("gdtf", std::string())); it != gdtf_files_.end())
            pkg.files["gdtf/" + it->first] = it->second;
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
    if (opened != media_opened_) {
        media_opened_ = opened;
        const bool live = kind == "video" || kind == "screen" || kind == "window" || kind == "ndi" || kind == "spout" ||
                          kind == "syphon";
        media_.open(live ? QString::fromStdString(kind) : QString(),
                    kind == "video" ? path_of_file() : QString::fromStdString(opened["name"].get<std::string>()));
    }
    if (kind == "video" && src.is_object())
        media_.set_playback(src.value("playing", true), src.value("loop", true), src.value("rate", 1.0));
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
        file = "media/" + info.fileName().toStdString();
        for (int n = 2;
             other_files_.contains(file) && other_files_[file] != std::vector<std::uint8_t>(bytes.begin(), bytes.end());
             ++n)
            file = "media/" + info.completeBaseName().toStdString() + "-" + std::to_string(n) + "." +
                   info.suffix().toStdString();
        other_files_[file] = {bytes.begin(), bytes.end()};
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
        scene = sv::mvr_import({reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())});
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    resolume_warnings_.clear(); // shown on the Project screen as the last import's warnings
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
    // Type text matches a library file name with or without ".gdtf", "Manufacturer@Name", or the type name.
    options.resolve = [this](const std::string &type, const std::string &mode) -> std::pair<std::string, std::string> {
        const QString t = QString::fromStdString(type).trimmed();
        for (const auto &[file, ft] : library_) {
            const QString f = QString::fromStdString(file);
            if (f.compare(t, Qt::CaseInsensitive) == 0 || f.compare(t + ".gdtf", Qt::CaseInsensitive) == 0 ||
                QString::fromStdString(ft.manufacturer + "@" + ft.name).compare(t, Qt::CaseInsensitive) == 0 ||
                QString::fromStdString(ft.name).compare(t, Qt::CaseInsensitive) == 0 ||
                QString::fromStdString(ft.short_name).compare(t, Qt::CaseInsensitive) == 0) {
                for (const auto &md : ft.modes)
                    if (QString::fromStdString(md.name).compare(QString::fromStdString(mode), Qt::CaseInsensitive) == 0)
                        return {file, md.name};
                return {file, ft.modes.empty() ? mode : ft.modes.front().name};
            }
        }
        return {{}, mode};
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
    return QSettings("SceneVisualiser", "Scene Visualiser").value(key, fallback);
}

void Session::setPreference(const QString &key, const QVariant &value) {
    if (key == "autoUpdate")
        return sv::updater::set_automatic(value.toBool());
    QSettings("SceneVisualiser", "Scene Visualiser").setValue(key, value);
    if (key == "crashReports")
        sv::crash::set_consent(value.toBool());
}

void Session::checkForUpdates() { sv::updater::check_now(); }

QVariantMap Session::app_info() {
    return {{"version", QCoreApplication::applicationVersion()},
            {"updates", sv::updater::supported()},
            {"crashUploads", sv::crash::uploads_supported()},
            {"crashFolder", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/crashes"}};
}
