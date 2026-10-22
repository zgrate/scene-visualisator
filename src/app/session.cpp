#include "session.h"

#include "render/test_pattern.h"

#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSession, "sv.session")

namespace {
Session *the_session = nullptr;
}

Session::Session(QObject *parent) : QObject(parent) {
    the_session = this;
    // Bundled generic types; project packages add their own gdtf/ entries once loading lands in the UI (M5).
    for (const QString &name : QDir(":/gdtf").entryList(QDir::Files)) {
        QFile f(":/gdtf/" + name);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = f.readAll();
        try {
            library_.emplace(name.toStdString(),
                             sv::gdtf::load_gdtf(std::span(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                                                           std::size_t(bytes.size()))));
        } catch (const std::exception &e) {
            qCWarning(lcSession) << "bundled GDTF" << name << "failed:" << e.what();
        }
    }
    refresh();
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

void Session::set_project(sv::json project) {
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
    snapshot_ = std::make_shared<const sv::json>(document_.data());
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
