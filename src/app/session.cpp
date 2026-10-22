#include "session.h"

#include "core/project_file.h"
#include "render/test_pattern.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <algorithm>

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
        per[QString::number(st.universe)] = QVariantMap{{"rate", st.rate_hz}, {"sources", sources}};
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
        const QStringList parts = text.trimmed().split('.');
        bool ok_u = false, ok_a = parts.size() == 2;
        const int u = parts.value(0).toInt(&ok_u), a = parts.value(1).toInt(&ok_a);
        if (!ok_u || !ok_a || u < 1 || u > 63999 || a < 1 || a > 512)
            return false;
        value = {{"universe", u}, {"address", a}};
    }
    return apply(json::array({{{"op", "replace"}, {"path", "/objects/" + std::to_string(i) + "/patch"}, {"value", value}}}),
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
    set_project(sv::new_project());
}

QString Session::openProject(const QUrl &url) {
    const std::filesystem::path path = url.toLocalFile().toStdU16String();
    try {
        sv::Package pkg = sv::load_package(path);
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
        path_ = path;
        set_project(std::move(pkg.project), QString::fromStdU16String(path.stem().u16string()));
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}

QString Session::saveProject(const QUrl &url) {
    std::filesystem::path path = url.isEmpty() ? path_ : std::filesystem::path(url.toLocalFile().toStdU16String());
    if (path.empty())
        return tr("No file chosen");
    if (path.extension() != ".svproj")
        path += ".svproj";
    sv::Package pkg{document_.data(), other_files_};
    for (const json &o : document_.data().at("objects")) // only the types the rig uses travel with it
        if (const auto it = gdtf_files_.find(o.value("gdtf", std::string())); it != gdtf_files_.end())
            pkg.files["gdtf/" + it->first] = it->second;
    try {
        sv::save_package(path, pkg);
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    document_.mark_saved();
    path_ = path;
    name_ = QString::fromStdU16String(path.stem().u16string());
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
