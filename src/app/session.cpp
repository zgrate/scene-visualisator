#include "session.h"

#include "core/interchange.h"
#include "render/test_pattern.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <climits>

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
    const auto it = lib.find(o.value("gdtf", std::string()));
    const sv::gdtf::Mode *m = it == lib.end() ? nullptr : it->second.mode(o.value("mode", std::string()));
    return m ? m->footprint : 0;
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
    composition_ = sv::render::make_composition(document_.data());
    std::vector<std::uint16_t> universes;
    for (const sv::json &o : document_.data().value("objects", sv::json::array()))
        if (const sv::json &p = o.value("patch", sv::json()); p.is_object())
            universes.push_back(p.value("universe", std::uint16_t(1)));
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
        else if (type == "truss")
            detail = number(o.value("length", 0.0)) + " m";
        else if (type == "ledWall")
            detail = number(o.value("width", 0.0)) + QStringLiteral(" × ") + number(o.value("height", 0.0));
        const std::string uuid = o.value("uuid", std::string());
        out.push_back(QVariantMap{{"uuid", QString::fromStdString(uuid)},
                                  {"name", QString::fromStdString(o.value("name", std::string()))},
                                  {"type", QString::fromStdString(type)},
                                  {"detail", detail},
                                  {"selected", selection_.contains(uuid)}});
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
        if (const json &p = o.value("patch", json()); p.is_object())
            u.insert(p.value("universe", 1));
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

void Session::edit(const QString &uuid, const QString &pointer, const QVariant &value) {
    const int i = index_of(uuid.toStdString());
    if (i < 0)
        return;
    const std::string path = "/objects/" + std::to_string(i) + pointer.toStdString();
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
        if (c.value("type", std::string()) == "fixture") {
            c["fixtureId"] = next_fixture_id(objects);
            if (c["patch"].is_object())
                c["patch"] = next_patch(objects, library_, footprint(library_, c));
        }
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
        const json &p = o.value("patch", json());
        if (!p.is_object() || p.value("universe", 1) != universe)
            continue;
        const bool sel = selection_.contains(o.value("uuid", std::string()));
        const int a = p.value("address", 1), fp = std::max(1, footprint(library_, o));
        for (int c = std::max(a, 1); c < std::min(a + fp, 513); ++c) {
            std::uint8_t &slot = out[std::size_t(c - 1)];
            slot = sel ? 3 : slot ? 2 : 1;
        }
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
        if (const json &p = o.value("patch", json()); p.is_object()) {
            auto &c = claims.try_emplace(p.value("universe", 1), std::array<int, 512>{}).first->second;
            const int a = p.value("address", 1), fp = std::max(1, footprint(library_, o));
            for (int ch = std::max(a, 1); ch < std::min(a + fp, 513); ++ch)
                ++c[std::size_t(ch - 1)];
        }
    QVariantList out;
    for (const json &o : objects) {
        if (o.value("type", std::string()) != "fixture")
            continue;
        const json &p = o.value("patch", json());
        const int fp = footprint(library_, o);
        bool conflict = false;
        if (p.is_object()) {
            const auto &c = claims[p.value("universe", 1)];
            const int a = p.value("address", 1);
            for (int ch = std::max(a, 1); ch < std::min(a + std::max(fp, 1), 513); ++ch)
                conflict |= c[std::size_t(ch - 1)] > 1;
        }
        const auto type = library_.find(o.value("gdtf", std::string()));
        const json &id = o.value("fixtureId", json());
        const std::string uuid = o.value("uuid", std::string());
        out.push_back(QVariantMap{
            {"uuid", QString::fromStdString(uuid)},
            {"fixtureId", id.is_number_integer() ? QVariant(id.get<int>()) : QVariant()},
            {"name", QString::fromStdString(o.value("name", std::string()))},
            {"type",
             QString::fromStdString(type == library_.end() ? o.value("gdtf", std::string()) : type->second.name)},
            {"mode", QString::fromStdString(o.value("mode", std::string()))},
            {"universe", p.is_object() ? QVariant(p.value("universe", 1)) : QVariant()},
            {"address", p.is_object() ? QVariant(p.value("address", 1)) : QVariant()},
            {"patch", patch_text(p)},
            {"footprint", fp},
            {"position", to_variant(o.value("position", json::array()))},
            {"conflict", conflict},
            {"overflow", p.is_object() && p.value("address", 1) + fp - 1 > 512},
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
        if (kind == "csv")
            text = sv::patch_csv(document_.data());
        else if (kind == "ma3")
            text = sv::ma3_patch_xml(document_.data());
        else if (kind == "resolume")
            text = sv::resolume_xml(document_.data());
        else if (kind == "obj" || kind == "objSelection") {
            if (kind == "objSelection" && selection_.empty())
                return tr("Select objects first");
            text = sv::render::export_obj(document_.data(), library_, *meshes_,
                                          kind == "obj" ? std::set<std::string>() : selection_);
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

QString Session::importResolume(const QUrl &url) {
    QString error;
    const QByteArray xml = read_file(url, error);
    if (!error.isEmpty())
        return error;
    try {
        apply(sv::resolume_import_patch(document_.data(), {xml.constData(), std::size_t(xml.size())}),
              "Import Resolume");
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
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
    // Copied into the package; a different file with the same name gets a numbered entry.
    const QFileInfo info(url.toLocalFile());
    std::string entry = "geometry/" + info.fileName().toStdString();
    for (int n = 2;
         other_files_.contains(entry) && other_files_[entry] != std::vector<std::uint8_t>(bytes.begin(), bytes.end());
         ++n)
        entry = "geometry/" + info.completeBaseName().toStdString() + "-" + std::to_string(n) + ".obj";
    other_files_[entry] = {bytes.begin(), bytes.end()};
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

void Session::rebuild_meshes() {
    auto lib = std::make_shared<sv::render::MeshLibrary>();
    for (const auto &[entry, bytes] : other_files_) {
        if (!entry.starts_with("geometry/") || !QString::fromStdString(entry).endsWith(".obj", Qt::CaseInsensitive))
            continue;
        try {
            std::vector<float> all;
            for (sv::ObjGroup &g : sv::parse_obj({reinterpret_cast<const char *>(bytes.data()), bytes.size()})) {
                all.insert(all.end(), g.vertices.begin(), g.vertices.end());
                (*lib)[entry + "#" + g.name] = sv::render::make_mesh(std::move(g.vertices));
            }
            (*lib)[entry] = sv::render::make_mesh(std::move(all));
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
