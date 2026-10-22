#include "session.h"

#include "core/project_file.h"

#include <QGuiApplication>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using sv::json;

namespace {
const json &object(Session &s, const QString &uuid) {
    for (const json &o : s.document().data().at("objects"))
        if (o["uuid"] == uuid.toStdString())
            return o;
    FAIL("no object " << uuid.toStdString());
    throw;
}
QString selected(Session &s) { return QString::fromStdString(*s.selection().begin()); }
} // namespace

TEST_CASE("Session adds fixtures at the next free ID and address, rolling to the next universe") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.addFixture("Generic@Spot_7ch.gdtf", "Default"); // demo patch uses 1.001-1.119, IDs 101..119
    const json &f = object(s, selected(s));
    CHECK(f["fixtureId"] == 120);
    CHECK(f["patch"] == json{{"universe", 1}, {"address", 120}});

    REQUIRE(s.setPatch(selected(s), "1.510"));
    s.addFixture("Generic@Spot_7ch.gdtf", "Default"); // 7 channels no longer fit after 516
    CHECK(object(s, selected(s))["patch"] == json{{"universe", 2}, {"address", 1}});
    CHECK_FALSE(s.setPatch(selected(s), "1.513"));
    CHECK(s.current()["overflow"] == false);
}

TEST_CASE("Session edits keep integers, duplicate re-addresses, delete and undo restore") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto count = [&] { return s.document().data()["objects"].size(); };
    const std::size_t before = count();
    s.addFixture("Generic@Wash_RGBW.gdtf", "Default");
    const QString uuid = selected(s);
    s.edit(uuid, "/fixtureId", 300.0); // QML hands numbers over as doubles
    CHECK(object(s, uuid)["fixtureId"].is_number_integer());
    s.edit(uuid, "/position/1", 5.5);
    CHECK(object(s, uuid)["position"] == json{0.0, 5.5, 0.0}); // replaced in place, not inserted

    s.duplicateSelection();
    const json &copy = object(s, selected(s));
    CHECK(copy["fixtureId"] == 301);
    CHECK(copy["patch"]["address"] == object(s, uuid)["patch"]["address"].get<int>() + 5);
    CHECK(count() == before + 2);

    s.selectUuid(uuid, true);
    s.deleteSelection();
    CHECK(count() == before);
    CHECK(s.selection().empty());
    s.undo();
    CHECK(count() == before + 2);
    CHECK(s.can_redo());
}

TEST_CASE("Session drag preview changes only the snapshot until commit") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const std::string uuid = s.document().data()["objects"][0]["uuid"];
    s.preview_move(uuid, {1, 6.5f, 2});
    CHECK(s.document().data()["objects"][0]["position"][0] == 0.0);
    CHECK((*s.snapshot())["objects"][0]["position"][0] == 1.0);
    s.commit_move();
    CHECK(s.document().data()["objects"][0]["position"] == json{1.0, 6.5, 2.0});
    CHECK(s.can_undo());
}

TEST_CASE("Session nudge moves every selected object in one undo step") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto position = [&](int i) { return s.document().data()["objects"][i]["position"]; };
    for (int i : {0, 1})
        s.selectUuid(QString::fromStdString(s.document().data()["objects"][i]["uuid"]), i > 0);
    s.nudgeSelection(0.25, 0, -0.25);
    CHECK(position(0) == json{0.25, 6.5, 1.25});
    CHECK(position(1) == json{0.25, 6.5, -2.75});
    s.undo();
    CHECK(position(0) == json{0.0, 6.5, 1.5});
    CHECK_FALSE(s.can_undo());
}

TEST_CASE("Session saves a package with the used GDTF types and opens it again") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / "rig.svproj";
    std::filesystem::remove(path);
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.edit(QString::fromStdString(s.document().data()["objects"][0]["uuid"]), "/name", "Upstage truss");
    CHECK(s.dirty());
    REQUIRE(s.saveProject(QUrl::fromLocalFile(QString::fromStdString((dir / "rig").string()))).isEmpty());
    CHECK_FALSE(s.dirty());
    CHECK(s.file_path() == QString::fromStdString(path.string()));

    const sv::Package pkg = sv::load_package(path);
    CHECK(pkg.files.contains("gdtf/Generic@Spot_7ch.gdtf"));
    CHECK(pkg.files.size() == 5); // the five bundled types the demo uses

    Session t(nullptr);
    REQUIRE(t.openProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(t.project_name() == "rig");
    CHECK(t.document().data()["objects"][0]["name"] == "Upstage truss");
    CHECK_FALSE(t.openProject(QUrl::fromLocalFile("/nonexistent.svproj")).isEmpty());
}

int main(int argc, char *argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) // also covers test discovery at build time
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv); // composition text needs fonts
    return Catch::Session().run(argc, argv);
}
