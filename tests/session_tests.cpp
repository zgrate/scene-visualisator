#include "session.h"

#include "ai_access.h"
#include "offline_render.h"
#include "view_keys.h"

#include "core/interchange.h"
#include "core/project_file.h"
#include "media/media_source.h"
#include "media/ndi.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <thread>

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

TEST_CASE("FR-2 addObject places a deck and a shape at a floor point") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    CHECK(s.addObject("deck", {{"position", QVariantList{2.0, 0.0, -1.0}}, {"height", 0.6}}).isEmpty());
    const json &d = object(s, selected(s));
    CHECK(d["type"] == "deck");
    CHECK(d["height"] == 0.6);
    CHECK(d["position"] == json{2.0, 0.0, -1.0});
    CHECK(s.document().history().back().label == "Add deck");
    CHECK(s.addObject("shape", {{"shape", "cylinder"}}).isEmpty());
    CHECK(object(s, selected(s))["shape"] == "cylinder");
    CHECK(object(s, selected(s))["name"] == "Cylinder");
    CHECK_FALSE(s.addObject("sphere", {}).isEmpty());
    s.edit("venue", "/width", 30.0);
    CHECK(s.document().data()["venue"]["width"] == 30.0);
    // A strip is patched after its fields merge, so a long strip does not overlap the next one.
    REQUIRE(s.addObject("strip", {{"pixels", 200}}).isEmpty());
    REQUIRE(s.addObject("strip", {}).isEmpty());
    s.selectUuids({});           // selected channels show as 3 and would hide a conflict
    for (int u = 1; u <= 8; ++u) // no channel is claimed twice (occupancy 2 = conflict)
        CHECK(std::ranges::count(s.occupancy(u), std::uint8_t(2)) == 0);
}

TEST_CASE("FR-4 FR-8 addFixtureArray adds N fixtures in one undo step, consecutive IDs and addresses rolling "
          "universes, parented to a truss") {
    Session s(nullptr);
    s.set_project(sv::new_project());
    s.addTruss();
    const QString truss = QString::fromStdString(*s.selection().begin());
    const std::size_t steps = s.document().cursor();
    const QVariantList origin{-2.0, -0.2, 0.0}, step{1.0, 0.0, 0.0};
    REQUIRE(s.addFixtureArray("Generic@Spot_7ch.gdtf", "Default", 5, origin, step, "1.500", truss).isEmpty());
    CHECK(s.document().cursor() == steps + 1);
    const json &objects = s.document().data()["objects"];
    REQUIRE(objects.size() == 6);
    CHECK(objects[1]["fixtureId"] == 101);
    CHECK(objects[5]["fixtureId"] == 105);
    CHECK(objects[1]["patch"] == json{{"universe", 1}, {"address", 500}});
    CHECK(objects[2]["patch"] == json{{"universe", 2}, {"address", 1}}); // 500 + 7 > 512
    CHECK(objects[3]["patch"] == json{{"universe", 2}, {"address", 8}});
    CHECK(objects[5]["position"] == json{2.0, -0.2, 0.0});
    CHECK(objects[5]["parent"] == truss.toStdString());
    CHECK(s.selection().size() == 5);
    // Empty start patch continues after the last fixture; no parent stays top level.
    REQUIRE(s.addFixtureArray("Generic@Spot_7ch.gdtf", "Default", 1, origin, step, "", "").isEmpty());
    CHECK(s.document().data()["objects"].back()["patch"] == json{{"universe", 2}, {"address", 29}});
    CHECK(s.document().data()["objects"].back()["parent"].is_null());
    CHECK_FALSE(s.addFixtureArray("nope.gdtf", "Default", 1, origin, step, "", "").isEmpty());
    CHECK_FALSE(s.addFixtureArray("Generic@Spot_7ch.gdtf", "Default", 0, origin, step, "", "").isEmpty());
    CHECK_FALSE(s.addFixtureArray("Generic@Spot_7ch.gdtf", "Default", 1, origin, step, "x", "").isEmpty());
    CHECK_FALSE(s.addFixtureArray("Generic@Spot_7ch.gdtf", "Default", 1, origin, step, "", "missing").isEmpty());
}

TEST_CASE("FR-5 addWallRun turns N points into N-1 wall shapes rotated about Y in one undo step") {
    Session s(nullptr);
    s.set_project(sv::new_project());
    const std::size_t steps = s.document().cursor();
    const QVariantList pts{QVariantList{0.0, 0.0}, QVariantList{4.0, 0.0}, QVariantList{4.0, -3.0}};
    REQUIRE(s.addWallRun(pts, 3, 0.2).isEmpty());
    CHECK(s.document().cursor() == steps + 1);
    CHECK(s.document().history().back().label == "Draw walls");
    const json &objects = s.document().data()["objects"];
    REQUIRE(objects.size() == 2);
    CHECK(objects[0]["size"] == json{4.0, 3.0, 0.2});
    CHECK(objects[0]["position"] == json{2.0, 0.0, 0.0});
    CHECK(objects[0]["rotation"][1] == 0.0);
    CHECK(objects[1]["size"][0] == 3.0);
    CHECK(objects[1]["position"] == json{4.0, 0.0, -1.5});
    CHECK(objects[1]["rotation"][1].get<double>() == Catch::Approx(90)); // -Z is +90 about Y
    CHECK(s.selection().size() == 2);
    CHECK(objects[1]["name"] == "Wall 2");
    REQUIRE(s.addWallRun({pts[1], pts[2]}, 3, 0.2).isEmpty()); // one segment per click keeps counting
    CHECK(s.document().data()["objects"].back()["name"] == "Wall 3");
    CHECK_FALSE(s.addWallRun({QVariantList{0.0, 0.0}}, 3, 0.2).isEmpty());
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

TEST_CASE("FR-5 preview_transform changes only the snapshot; commit writes position, rotation and scale as one step") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const std::string uuid = s.document().data()["objects"][1]["uuid"];
    const std::size_t steps = s.document().cursor();
    s.preview_transform(uuid, {{"position", {1, 6.5, 2}}});
    s.preview_transform(uuid, {{"position", {1, 6.5, 2}}, {"rotation", {0, 45, 0}}, {"scale", {2, 2, 2}}});
    CHECK(s.document().data()["objects"][1]["position"][0] == 0.0);
    CHECK((*s.snapshot())["objects"][1]["rotation"][1] == 45);
    s.commit_transform("Rotate");
    const json &o = s.document().data()["objects"][1];
    CHECK(o["position"] == json{1, 6.5, 2});
    CHECK(o["rotation"] == json{0, 45, 0});
    CHECK(o["scale"] == json{2, 2, 2});
    CHECK(s.document().cursor() == steps + 1);
    CHECK(s.document().history().back().label == "Rotate");
    s.preview_transform(uuid, {{"position", {9, 9, 9}}});
    s.cancel_transform();
    CHECK((*s.snapshot())["objects"][1]["position"] == json{1, 6.5, 2});
    s.commit_transform("Move"); // nothing pending
    CHECK(s.document().cursor() == steps + 1);
}

TEST_CASE("controls preference defaults to blender and notifies on change") {
    Session s(nullptr);
    CHECK(Session::controls() == "blender");
    int notified = 0;
    QObject::connect(&s, &Session::changed, [&] { ++notified; });
    Session::setPreference("controls", "maya");
    CHECK(Session::controls() == "maya");
    Session::setPreference("controls", "blender");
    CHECK(notified == 2);
}

TEST_CASE("Session nudge moves every selected object in one undo step") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto position = [&](int i) { return s.document().data()["objects"][i + 1]["position"]; }; // after the deck
    for (int i : {1, 2})
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
    CHECK(s.file_path() == QString::fromStdString(path.generic_string()));

    const sv::Package pkg = sv::load_package(path);
    CHECK(pkg.files.contains("gdtf/Generic@Spot_7ch.gdtf"));
    CHECK(pkg.files.size() == 5); // the five bundled types the demo uses

    Session t(nullptr);
    REQUIRE(t.openProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(t.project_name() == "rig");
    CHECK(t.document().data()["objects"][0]["name"] == "Upstage truss");
    CHECK_FALSE(t.openProject(QUrl::fromLocalFile("/nonexistent.svproj")).isEmpty());
}

TEST_CASE("Session re-addresses the selection in fixture ID order and flags conflicts") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    CHECK(s.conflict_count() == 0);
    const QVariantList rows = s.patch_rows();
    REQUIRE(rows.size() == 19);
    const QString spot2 = rows[1].toMap()["uuid"].toString(), spot1 = rows[0].toMap()["uuid"].toString();
    s.selectUuids({spot2, spot1});
    REQUIRE(s.readdressSelection("2.510").isEmpty());
    CHECK(object(s, spot1)["patch"] == json({{"universe", 3}, {"address", 1}})); // 7 channels do not fit at 510
    CHECK(object(s, spot2)["patch"] == json({{"universe", 3}, {"address", 8}}));
    CHECK_FALSE(s.readdressSelection("1.600").isEmpty());

    s.selectUuids({spot1});
    REQUIRE(s.readdressSelection("1.008").isEmpty()); // on top of Spot 2's old range
    CHECK(s.conflict_count() == 0);                   // Spot 2 moved away, so 1.008 is free again
    s.selectUuids({spot2});
    REQUIRE(s.readdressSelection("1.010").isEmpty());
    CHECK(s.conflict_count() == 3); // 1.010-016 overlaps Spot 1 (008-014) and Spot 3 (015-021)
    const auto occ = s.occupancy(1);
    CHECK(occ[7] == 1); // Spot 1 alone
    CHECK(occ[9] == 3); // Spot 2 (selected) wins the display
    CHECK(occ[200] == 0);
}

TEST_CASE("Session imports an OBJ as set pieces, renders it and exports it again") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto obj = dir / "set.obj";
    std::ofstream(obj) << "v 0 0 0\nv 100 0 0\nv 100 0 100\nv 0 0 100\ng Riser\nf 1 2 3 4\ng Ramp\nf 1 2 3\n";
    Session s(nullptr);
    s.set_project(sv::new_project());
    REQUIRE(s.importObj(QUrl::fromLocalFile(QString::fromStdString(obj.string())), "cm", true, true).isEmpty());
    const json &objects = s.document().data()["objects"];
    REQUIRE(objects.size() == 2);
    CHECK(objects[0]["type"] == "setPiece");
    CHECK(objects[0]["geometry"] == "geometry/set.obj");
    CHECK(objects[1]["group"] == "Ramp");
    CHECK(objects[0]["scale"][0] == 0.01);
    CHECK(s.selection_count() == 2);
    REQUIRE(s.meshes()->contains("geometry/set.obj#Riser"));

    const auto scene = sv::render::build_scene(*s.snapshot(), {}, {}, 0, *s.meshes());
    REQUIRE(scene.meshes.size() == 2);
    // Z-up file, rotated -90 about X: the riser's +Z (100 cm) edge becomes 1 m up.
    CHECK(scene.meshes[0].model.map(QVector3D(0, 0, 100)).y() == Catch::Approx(1.0f).margin(1e-4));

    const auto out = dir / "scene.obj";
    REQUIRE(s.exportFile("obj", QUrl::fromLocalFile(QString::fromStdString(out.string()))).isEmpty());
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto groups = sv::parse_obj(text);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].name == "Riser");
    CHECK(groups[0].vertices.size() == 2 * 3 * 6);
    CHECK(s.exportFile("nope", QUrl::fromLocalFile("/tmp/x")).size() > 0);
}

TEST_CASE("Session imports Arena outputs and strips, edits their params and exports them (FR-24)") {
    const QString arena = QStringLiteral(SV_SOURCE_DIR "/tests/fixtures/resolume/arena-7.17-futro-ajki26.xml");
    Session s(nullptr);
    s.set_project(sv::new_project());
    REQUIRE(s.importResolume(QUrl::fromLocalFile(arena)).isEmpty());
    CHECK(s.outputs().size() == 10);
    CHECK_FALSE(s.resolume_warnings().isEmpty());

    // Every universe a strip rolls into is subscribed and occupied.
    const json &objects = s.document().data()["objects"];
    const auto strip = std::ranges::find(objects, json("pixelStrip"), [](const json &o) { return o["type"]; });
    REQUIRE(strip != objects.end());
    const auto last = sv::strip_pixel_address(*strip, (*strip)["pixels"].get<int>() - 1);
    CHECK(s.universes().contains(last.universe));
    CHECK(s.occupancy(last.universe)[std::size_t(last.address - 1)] != 0);
    CHECK(s.patch_rows().size() == 41);

    // A Lumiverse's Art-Net target is a param of the output.
    QString lumiverse;
    for (const QVariant &o : s.outputs())
        if (o.toMap()["kind"] == "lumiverse")
            lumiverse = o.toMap()["uuid"].toString();
    REQUIRE_FALSE(lumiverse.isEmpty());
    const QString key = "OutputDevice/OutputDeviceDmx/DmxOutputParams/TargetIP";
    bool found = false;
    for (const QVariant &p : s.resolumeParams(lumiverse))
        found |= p.toMap()["key"] == key;
    CHECK(found);
    CHECK_FALSE(s.resolumeParams("composition").isEmpty());
    s.setResolumeParam(lumiverse, key, "10.0.0.7");
    s.edit(lumiverse, "/name", "Front LED");

    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto out = dir / "arena.xml";
    REQUIRE(s.exportFile("resolume", QUrl::fromLocalFile(QString::fromStdString(out.string()))).isEmpty());
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text.find("10.0.0.7") != std::string::npos);
    CHECK(text.find("Front LED") != std::string::npos);
}

TEST_CASE("Session autosaves unsaved edits and offers them back after a crash (AT-6)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / "crash.svproj";
    std::filesystem::remove(path);
    std::filesystem::remove(sv::autosave_path(path));
    const QUrl url = QUrl::fromLocalFile(QString::fromStdString(path.string()));
    {
        Session s(nullptr);
        s.set_project(sv::demo_project());
        REQUIRE(s.saveProject(url).isEmpty());
        s.autosave(); // nothing unsaved: no file
        CHECK_FALSE(std::filesystem::exists(sv::autosave_path(path)));
        s.edit(QString::fromStdString(s.document().data()["objects"][0]["uuid"]), "/name", "After the save");
        s.autosave();
        CHECK(std::filesystem::exists(sv::autosave_path(path)));
    } // "crash": the session goes away without saving
    Session t(nullptr);
    REQUIRE(t.openProject(url).isEmpty());
    REQUIRE_FALSE(t.recovery_file().isEmpty());
    CHECK(t.document().data()["objects"][0]["name"] != "After the save"); // the valid saved package
    REQUIRE(t.recover().isEmpty());
    CHECK(t.document().data()["objects"][0]["name"] == "After the save");
    CHECK(t.dirty());
    CHECK(t.file_path() == QString::fromStdString(path.generic_string()));
    REQUIRE(t.saveProject({}).isEmpty());
    CHECK_FALSE(std::filesystem::exists(sv::autosave_path(path)));

    Session u(nullptr);
    REQUIRE(u.openProject(url).isEmpty());
    CHECK(u.recovery_file().isEmpty());
}

TEST_CASE(
    "Session composition: mapping grid outlines slices, stills are packaged, video frames arrive (FR-13, FR-14)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_media";
    std::filesystem::create_directories(dir);
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const json wall =
        *std::ranges::find_if(s.document().data().at("objects"), [](const json &o) { return o["type"] == "ledWall"; });
    const int sx = wall["slice"]["x"], sy = wall["slice"]["y"], sh = wall["slice"]["h"];

    s.edit("composition", "/source", QVariantMap{{"kind", "grid"}});
    CHECK(s.composition().pixelColor(sx + 2, sy + sh / 3) == QColor(Qt::white)); // slice outline
    CHECK(s.composition().pixelColor(sx + 40, sy + sh / 3).lightness() < 128);   // grid background

    const QString png = QString::fromStdString((dir / "green.png").string());
    QImage green(32, 18, QImage::Format_RGB32);
    green.fill(Qt::green);
    REQUIRE(green.save(png));
    REQUIRE(s.setMediaFile("still", QUrl::fromLocalFile(png)).isEmpty());
    CHECK(s.document().data()["composition"]["source"]["file"] == "media/green.png");
    const QImage &comp = s.composition();
    CHECK(comp.pixelColor(comp.width() / 2, comp.height() / 2) == QColor(Qt::green));
    CHECK_FALSE(
        s.setMediaFile("still", QUrl::fromLocalFile(SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4")).isEmpty());

    const auto path = dir / "media.svproj";
    REQUIRE(s.saveProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(sv::load_package(path).files.contains("media/green.png"));
    Session t(nullptr);
    REQUIRE(t.openProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(t.composition().pixelColor(t.composition().width() / 2, 10) == QColor(Qt::green));

    REQUIRE(t.setMediaFile("video", QUrl::fromLocalFile(SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4")).isEmpty());
    CHECK(t.document().data()["composition"]["source"]["file"] == "media/testsrc.mp4");
    for (int i = 0; i < 100 && !t.media().status().signal; ++i) // up to 5 s for the decoder to start
    {
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
    }
    REQUIRE(t.media().status().signal);
    CHECK(t.media().status().size == QSize(160, 90));
    quint64 serial = 0;
    CHECK_FALSE(t.media().latest(serial).isNull());
    CHECK(t.media().latest(serial).isNull()); // nothing newer yet
    t.undo();                                 // back to the still: the live source stops
    CHECK_FALSE(t.media().active());
}

namespace {
// Runs one AI tool as the MCP server would: its result, with isError turned into an exception.
json tool(const std::vector<sv::mcp::Tool> &tools, const std::string &name, const json &args = json::object()) {
    for (const auto &t : tools)
        if (t.name == name) {
            json r = t.run(args);
            if (r.value("isError", false))
                throw std::runtime_error(r["content"][0]["text"].get<std::string>());
            return r.contains("structuredContent") ? r["structuredContent"] : r["content"][0];
        }
    FAIL("no tool " << name);
    throw;
}
} // namespace

TEST_CASE("Venue haze is editable, read back by Session.venue and get_project") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const std::size_t steps = s.document().cursor();
    s.edit("venue", "/haze", 0.1);
    CHECK(s.document().cursor() == steps + 1);
    CHECK(s.venue()["haze"].toDouble() == Catch::Approx(0.1));
    const json venue = tool(make_mcp_tools(s), "get_project")["venue"];
    CHECK(venue["haze"] == 0.1);
    CHECK(venue["height"] == s.document().data()["venue"]["height"]);
}

TEST_CASE("AI tools: every edit is one undo step labelled AI, errors come back as tool errors") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto tools = make_mcp_tools(s);
    const std::size_t steps = s.document().cursor();

    const json fixtures = tool(tools, "list_objects", {{"type", "fixture"}})["objects"];
    REQUIRE(fixtures.size() == 19);
    const std::string uuid = fixtures[0]["uuid"];
    tool(tools, "edit_object", {{"uuid", uuid}, {"pointer", "/position/1"}, {"value", 7.5}});
    CHECK(object(s, QString::fromStdString(uuid))["position"][1] == 7.5);
    CHECK(s.document().cursor() == steps + 1);
    CHECK(s.document().history().back().label == "AI: Edit /position/1");

    CHECK(tool(tools, "add_object", {{"type", "deck"}})["type"] == "deck");
    tool(tools, "undo");
    const json wall = tool(tools, "add_object", {{"type", "wall"}, {"fields", {{"name", "AI wall"}}}});
    CHECK(wall["name"] == "AI wall");
    CHECK(s.document().history()[s.document().cursor() - 1].label.starts_with("AI: "));
    tool(tools, "set_patch", {{"uuid", uuid}, {"patch", "3.101"}});
    CHECK(object(s, QString::fromStdString(uuid))["patch"] == json({{"universe", 3}, {"address", 101}}));
    CHECK(s.document().cursor() == steps + 3);

    CHECK(tool(tools, "undo")["undone"].get<std::string>().starts_with("AI: "));
    CHECK(tool(tools, "undo")["undone"] == "AI: Add LED wall");
    CHECK(s.document().cursor() == steps + 1);
    const json walls = tool(tools, "add_wall_run", {{"points", {{0, 0}, {4, 0}, {4, 3}, {0, 3}}}})["walls"];
    CHECK(walls.size() == 3);
    CHECK(object(s, QString::fromStdString(walls[0]))["shape"] == "box");
    CHECK(tool(tools, "undo")["undone"].get<std::string>().starts_with("AI: "));
    CHECK_THROWS(tool(tools, "add_wall_run", {{"points", {{0, 0}}}}));

    const json array = tool(tools, "add_fixture_array",
                            {{"gdtf", "Generic@Spot_7ch.gdtf"},
                             {"mode", s.modes("Generic@Spot_7ch.gdtf")[0].toStdString()},
                             {"count", 4},
                             {"patch", "2.500"}})["fixtures"];
    REQUIRE(array.size() == 4);
    CHECK(array[0]["patch"] == json({{"universe", 2}, {"address", 500}}));
    CHECK(array[1]["patch"] == json({{"universe", 3}, {"address", 1}})); // 7 channels do not fit after 2.507
    CHECK(array[3]["fixtureId"].get<int>() == array[0]["fixtureId"].get<int>() + 3);
    CHECK(tool(tools, "undo")["undone"].get<std::string>().starts_with("AI: "));

    json spec = tool(tools, "get_fixture_spec", {{"gdtf", "Generic@Spot_7ch.gdtf"}});
    CHECK(spec["bundled"] == true);
    CHECK_THROWS_WITH(tool(tools, "save_fixture_type", {{"spec", spec}}),
                      "Bundled types are read-only; change the name");
    spec["name"] = "AI Spot";
    spec["manufacturer"] = "Acme";
    CHECK(tool(tools, "save_fixture_type", {{"spec", spec}})["gdtf"] == "Acme@AI_Spot.gdtf");
    CHECK(tool(tools, "get_fixture_spec", {{"gdtf", "Acme@AI_Spot.gdtf"}})["name"] == "AI Spot");
    CHECK_THROWS(tool(tools, "get_fixture_spec", {{"gdtf", "nope.gdtf"}}));

    s.addTruss(); // user edits keep their plain labels
    CHECK_FALSE(s.document().history().back().label.starts_with("AI: "));

    CHECK_THROWS_WITH(tool(tools, "get_object", {{"uuid", "nope"}}), "No object, output or composition with uuid nope");
    CHECK_THROWS(tool(tools, "apply_patch", {{"patch", {{{"op", "remove"}, {"path", "/missing"}}}}}));
    CHECK(s.document().history().back().label == "Add truss"); // the failed patch left no step
    CHECK_THROWS_WITH(tool(tools, "screenshot"), "No window (the app runs without one)");
    CHECK(tool(tools, "get_fixture", {{"uuid", uuid}})["channels"].size() > 0);

    // File and debug tools exist but are switched off by default.
    const auto open = std::ranges::find(tools, std::string("open_project"), &sv::mcp::Tool::name);
    const auto log = std::ranges::find(tools, std::string("get_log"), &sv::mcp::Tool::name);
    CHECK_FALSE(open->available());
    CHECK_FALSE(log->available());
    s.setDebugMode(true, false);
    CHECK(log->available());
    Session::append_log("info sv.test ai tools marker"); // main.cpp's message handler does this in the app
    CHECK(tool(tools, "get_log", {{"filter", "AI TOOLS MARKER"}})["lines"] == json{"info sv.test ai tools marker"});
    s.setDebugMode(false, false);
}

TEST_CASE("AI tools filter objects, describe types and beams, list files and reply with JSON") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto tools = make_mcp_tools(s);

    const json spots = tool(tools, "list_objects", {{"gdtf", "Generic@Spot_7ch.gdtf"}, {"name", "spot"}})["objects"];
    REQUIRE_FALSE(spots.empty());
    for (const json &o : spots)
        CHECK(o["gdtf"] == "Generic@Spot_7ch.gdtf");
    CHECK(tool(tools, "list_objects", {{"type", "fixture"}, {"patched", false}})["objects"].empty());
    CHECK(tool(tools, "list_objects", {{"name", "no such name"}})["objects"].empty());

    const json type = tool(tools, "get_fixture_type", {{"gdtf", "Generic@Spot_7ch.gdtf"}});
    CHECK(type["hasBeam"] == true);
    REQUIRE(type["modes"].size() >= 1);
    CHECK(type["modes"][0]["channels"][0].contains("functions"));
    CHECK_THROWS(tool(tools, "get_fixture_type", {{"gdtf", "Generic@Spot_7ch.gdtf"}, {"mode", "nope"}}));

    const std::string uuid = spots[0]["uuid"];
    const json fixture = tool(tools, "get_fixture", {{"uuid", uuid}});
    CHECK(fixture["decoded"]["heads"].empty());
    CHECK(fixture["channels"][0].contains("head"));

    // set_fixture encodes through the GDTF: Dimmer 1 is 255 on the Dimmer channel's address.
    const json set = tool(tools, "set_fixture", {{"uuid", uuid}, {"attributes", {{"Dimmer", 1}, {"Nope", 1}}}});
    const json &written = set["fixtures"][0];
    CHECK(written["unknown"] == json{"Nope"});
    int dimmer_address = 0;
    for (const json &c : fixture["channels"])
        if (c["attribute"] == "Dimmer")
            dimmer_address = c["address"];
    REQUIRE(dimmer_address > 0);
    CHECK(written["channels"][std::to_string(dimmer_address)] == 255);
    CHECK(tool(tools, "clear_dmx")["cleared"] == "all");
    CHECK(tool(tools, "apply_patch", {{"patch", json::array()}})["applied"] == 0);

    CHECK(tool(tools, "get_project").contains("recentFiles"));
    CHECK(tool(tools, "get_render")["stages"].contains("draw"));
    CHECK(tool(tools, "get_render", {{"preview", true}}).contains("frames")); // no frame yet: plain status
    CHECK_THROWS_WITH(tool(tools, "frame", {{"uuids", {uuid}}}), "No viewport (the app runs without a window)");

    s.setDebugMode(true, false);
    tool(tools, "set_generator", {{"pattern", "full"}});
    const json beams = tool(tools, "get_beams")["beams"];
    for (const json &b : beams)
        CHECK(b.contains("hit"));
    CHECK(tool(tools, "get_beams", {{"uuid", "nope"}})["beams"].empty());
    tool(tools, "set_generator", {{"pattern", "off"}});
    s.setDebugMode(false, false);

    const auto list = std::ranges::find(tools, std::string("list_files"), &sv::mcp::Tool::name);
    REQUIRE(list != tools.end());
    const auto dir = std::filesystem::temp_directory_path() / "sv_list_files";
    std::filesystem::create_directories(dir / "sub");
    std::ofstream(dir / "a.mvr") << "x";
    std::ofstream(dir / "b.txt") << "y";
    const json entries = tool(tools, "list_files", {{"path", dir.string()}, {"pattern", "*.mvr"}})["entries"];
    REQUIRE(entries.size() == 2); // folders always show, so the caller can walk down
    CHECK(entries[0]["folder"] == true);
    CHECK(entries[1]["name"] == "a.mvr");
    CHECK_THROWS(tool(tools, "list_files", {{"path", (dir / "missing").string()}}));
}

TEST_CASE("AI tools called from another thread run on the UI thread and wait for it") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto tools = make_mcp_tools(s);
    json result;
    std::thread caller([&] { result = tool(tools, "add_object", {{"type", "truss"}}); });
    QElapsedTimer t;
    t.start();
    while (result.is_null() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    caller.join();
    CHECK(result["type"] == "truss");
    CHECK(s.document().history().back().label.starts_with("AI: "));
}

TEST_CASE("AI set_dmx holds values that get_dmx reads back merged; clear_dmx releases them") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    try {
        s.dmx().start();
    } catch (const std::exception &e) {
        SKIP("DMX ports busy: " << e.what());
    }
    const auto tools = make_mcp_tools(s);
    CHECK(tool(tools, "set_dmx", {{"universe", 1}, {"address", 10}, {"values", {255, 7}}})["patched"] == true);
    CHECK_THROWS(tool(tools, "set_dmx", {{"universe", 1}, {"address", 512}, {"values", {1, 2}}}));
    const auto read = [&] { // status lists the universe only once the network thread has subscribed it
        try {
            return tool(tools, "get_dmx", {{"universe", 1}, {"from", 10}, {"count", 2}})["values"];
        } catch (const std::runtime_error &) {
            return json();
        }
    };
    QElapsedTimer t;
    t.start();
    while (read() != json({255, 7}) && t.elapsed() < 3000)
        QThread::msleep(20);
    CHECK(read() == json({255, 7}));
    tool(tools, "clear_dmx");
    t.restart();
    while (read() != json({0, 0}) && t.elapsed() < 3000)
        QThread::msleep(20);
    CHECK(read() == json({0, 0}));
    CHECK_THROWS(tool(tools, "get_dmx", {{"universe", 40}}));
}

int main(int argc, char *argv[]) {
    QStandardPaths::setTestModeEnabled(true); // the untitled autosave goes to a test location
    // Recent files and preferences go to a scratch folder, never the developer's own settings.
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       QString::fromStdString((std::filesystem::temp_directory_path() / "sv_settings").string()));
    QSettings("SceneVisualiser", "Scene Visualiser").clear();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) // also covers test discovery at build time
        qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) // offscreen finds no fonts on Windows by itself
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QGuiApplication app(argc, argv); // composition text needs fonts
    return Catch::Session().run(argc, argv);
}

TEST_CASE("Session exports MVR and merges it back in one undo step, renaming colliding UUIDs (FR-22)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const QUrl mvr = QUrl::fromLocalFile(QString::fromStdString((dir / "rig.mvr").string()));
    Session s(nullptr);
    s.set_project(sv::demo_project());
    REQUIRE(s.exportFile("mvr", mvr).isEmpty());
    const std::size_t before = s.document().data()["objects"].size();

    Session t(nullptr);
    t.set_project(sv::new_project());
    REQUIRE(t.importMvr(mvr).isEmpty());
    const std::size_t imported = t.document().data()["objects"].size();
    CHECK(imported >= before - 1); // no pixel strips or cameras in the demo besides what MVR carries
    CHECK(t.fixture_types().contains("Generic@Spot_7ch.gdtf"));
    CHECK_FALSE(t.meshes()->empty()); // generated .glb walls and trusses load as meshes
    // Again into the same project: same count added, all UUIDs unique, one undo removes the lot.
    REQUIRE(t.importMvr(mvr).isEmpty());
    std::set<std::string> uuids;
    for (const json &o : t.document().data()["objects"])
        uuids.insert(o["uuid"].get<std::string>());
    CHECK(uuids.size() == 2 * imported);
    t.undo();
    CHECK(t.document().data()["objects"].size() == imported);
    CHECK(t.importMvr(QUrl::fromLocalFile("/nonexistent.mvr")).size() > 0);
}

TEST_CASE("Session previews a CSV, maps its columns and imports fixtures by type name (FR-23)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto csv = dir / "patch.csv";
    std::ofstream(csv) << "Fixture ID;Label;Model;Mode;Patch;X;Y;Z\n"
                          "201;Wash A;Wash RGBW;;3.001;100;0;450\n"
                          "202;Wash B;Generic@Wash_RGBW;;3.011;-100;0;450\n";
    const QUrl url = QUrl::fromLocalFile(QString::fromStdString(csv.string()));
    Session s(nullptr);
    s.set_project(sv::new_project());
    const QVariantMap preview = s.csvPreview(url);
    CHECK(preview["count"] == 2);
    QVariantMap mapping = preview["mapping"].toMap();
    CHECK(mapping["fixtureId"] == 0);
    CHECK(mapping["patch"] == 4);
    REQUIRE(s.importCsv(url, mapping, "cm", true).isEmpty());
    const json &objects = s.document().data()["objects"];
    REQUIRE(objects.size() == 2);
    CHECK(objects[1]["gdtf"] == "Generic@Wash_RGBW.gdtf");
    CHECK(objects[1]["patch"] == json{{"universe", 3}, {"address", 11}});
    CHECK(objects[0]["position"][1].get<double>() == Catch::Approx(4.5));
    // Same file again updates by fixture ID instead of adding.
    REQUIRE(s.importCsv(url, mapping, "cm", true).isEmpty());
    CHECK(s.document().data()["objects"].size() == 2);
    CHECK(s.csvPreview(QUrl::fromLocalFile("/nonexistent.csv")).contains("error"));
}

TEST_CASE("Session exports FBX and imports it back as set pieces under groups, one undo step (FR-25, AT-4)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const QUrl fbx = QUrl::fromLocalFile(QString::fromStdString((dir / "rig.fbx").string()));
    Session s(nullptr);
    json rig = sv::demo_project();
    rig["objects"].push_back(sv::new_camera("FOH camera"));
    rig["layout"]["cameraBookmarks"] =
        json::array({{{"target", {0, 2, 0}}, {"yaw", 0}, {"pitch", 10}, {"distance", 18}, {"fov", 45}}});
    s.set_project(rig);
    REQUIRE(s.exportFile("fbx", fbx).isEmpty());
    // Also the OBJ of the same rig, so tools/blender_check.py can open both (CI runs it after the tests).
    REQUIRE(s.exportFile("obj", QUrl::fromLocalFile(QString::fromStdString((dir / "rig.obj").string()))).isEmpty());
    CHECK(s.exportFile("fbxSelection", fbx).size() > 0); // nothing selected

    Session t(nullptr);
    t.set_project(sv::new_project());
    REQUIRE(t.importFbx(fbx).isEmpty());
    const json &objects = t.document().data()["objects"];
    std::map<std::string, int> types;
    std::set<std::string> names;
    for (const json &o : objects)
        ++types[o["type"].get<std::string>()], names.insert(o["name"].get<std::string>());
    CHECK(types["group"] >= 3); // Trusses, Walls, Fixtures; the venue mesh sits at the root
    CHECK(types["setPiece"] > 10);
    CHECK(names.contains("Trusses"));
    for (const json &o : objects)
        if (o["type"] == "setPiece") {
            CHECK((o["parent"].is_string() || o["name"] == "Venue")); // object meshes sit under their group null
            CHECK(t.meshes()->contains(o["geometry"].get<std::string>() + "#" + o["group"].get<std::string>()));
        }
    // World-space vertices come back where they were: the scene bounds match within a millimetre.
    const auto bounds = [](const sv::render::RenderScene &scene) {
        float hi = -1e9f;
        for (const auto &m : scene.meshes)
            for (std::size_t i = 1; i < m.mesh->vertices.size(); i += 6)
                hi = std::max(
                    hi,
                    m.model.map(QVector3D(m.mesh->vertices[i - 1], m.mesh->vertices[i], m.mesh->vertices[i + 1])).y());
        return hi;
    };
    const auto scene = sv::render::build_scene(*t.snapshot(), {}, {}, 0, *t.meshes());
    CHECK(bounds(scene) > 2); // trusses hang well above the deck
    t.undo();
    CHECK(t.document().data()["objects"].empty());
    CHECK(t.importFbx(QUrl::fromLocalFile("/nonexistent.fbx")).size() > 0);
}

namespace {
// Plan and front elevation of the demo rig without lettering, against tests/golden/<kind>.png (FR-19).
// SV_UPDATE_GOLDEN=1 or a missing golden rewrites it.
void check_drawing_golden(Session &s, const QString &kind) {
    sv::render::DrawingSheet sheet = s.drawing_sheet(kind);
    sheet.text = false;
    const QSizeF mm = sv::render::sheet_size_mm(sheet);
    QImage actual(QSize(int(mm.width() * 2), int(mm.height() * 2)), QImage::Format_RGBA8888);
    actual.fill(Qt::black);
    QPainter p(&actual);
    p.scale(2, 2);
    sv::render::paint_drawing(p, s.document().data(), *s.library_snapshot(), *s.meshes(), sheet);
    p.end();
    const QString golden = QStringLiteral(SV_SOURCE_DIR "/tests/golden/%1.png").arg(kind);
    if (qEnvironmentVariableIntValue("SV_UPDATE_GOLDEN") || !QFile::exists(golden)) {
        REQUIRE(actual.save(golden));
        return;
    }
    const QImage expected = QImage(golden).convertToFormat(QImage::Format_RGBA8888);
    REQUIRE(expected.size() == actual.size());
    long bad = 0;
    for (int y = 0; y < actual.height(); ++y) {
        const uchar *a = actual.constScanLine(y), *e = expected.constScanLine(y);
        for (int x = 0; x < actual.width() * 4; x += 4)
            bad += std::max({std::abs(a[x] - e[x]), std::abs(a[x + 1] - e[x + 1]), std::abs(a[x + 2] - e[x + 2])}) > 48;
    }
    INFO(kind.toStdString() << " pixels off: " << bad);
    CHECK(bad < actual.width() * actual.height() / 500);
}
} // namespace

TEST_CASE("Session drawings: sheet settings are one undo step, plan and front match their goldens (FR-19)") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    CHECK(s.drawing("front").value("kind").toString() == "front");
    CHECK_FALSE(s.document().data().contains("drawings"));

    s.setDrawing("front", "scale", 100.0);
    s.setDrawing("plan", "sheet", "A4");
    const json &drawings = s.document().data().at("drawings");
    REQUIRE(drawings.size() == 2);
    CHECK(drawings[0]["scale"] == 100);
    CHECK(drawings[0]["scale"].is_number_integer());
    CHECK(s.drawing_sheet("plan").paper == "A4");
    s.undo();
    CHECK(s.drawing_sheet("plan").paper == "A3");
    s.undo();
    CHECK_FALSE(s.document().data().contains("drawings"));

    check_drawing_golden(s, "plan");
    check_drawing_golden(s, "front");
}

TEST_CASE("Session drawings: the legend fills the strip left of the title block, the schedule narrows the drawing "
          "(FR-19)") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const auto render = [&](sv::render::DrawingSheet sheet, QTransform *to_page = nullptr) {
        const QSizeF mm = sv::render::sheet_size_mm(sheet);
        QImage image(QSize(int(mm.width()), int(mm.height())), QImage::Format_RGBA8888);
        image.fill(Qt::white);
        QPainter p(&image);
        const QTransform t =
            sv::render::paint_drawing(p, s.document().data(), *s.library_snapshot(), *s.meshes(), sheet);
        p.end();
        if (to_page)
            *to_page = t;
        return image;
    };
    const auto ink_in = [](const QImage &image, QRect r) {
        int n = 0;
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                n += qGray(image.pixel(x, y)) < 240;
        return n;
    };
    sv::render::DrawingSheet sheet = s.drawing_sheet("plan");
    CHECK(sheet.legend);
    CHECK_FALSE(sheet.schedule);
    const QRect strip(20, 297 - 30, 200, 20); // A3: left of the 150 mm title block, above the frame's bottom
    const int with_legend = ink_in(render(sheet), strip);
    sheet.legend = false;
    CHECK(with_legend > ink_in(render(sheet), strip) + 20);

    QTransform plain, narrow;
    render(sheet, &plain);
    s.setDrawing("plan", "schedule", true);
    sheet = s.drawing_sheet("plan");
    REQUIRE(sheet.schedule);
    render(sheet, &narrow);
    // The demo rig is height-bound on A3, so the scale stays and the drawing moves left into the narrower area.
    CHECK(narrow.m11() <= plain.m11());
    CHECK(narrow.dx() < plain.dx() - 20);
}

TEST_CASE("Session exports drawings to a one-page A3 landscape PDF and to SVG (FR-19)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.setDrawing("front", "dimensions",
                 QVariantList{QVariantMap{{"a", QVariantList{-4, -6}}, {"b", QVariantList{4, -6}}}});

    const QString pdf = QString::fromStdString((dir / "front.pdf").string());
    REQUIRE(s.exportDrawing("front", QUrl::fromLocalFile(pdf)).isEmpty());
    QFile f(pdf);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromLatin1(f.readAll());
    CHECK(text.startsWith("%PDF"));
    CHECK(text.count(QRegularExpression("/Type\\s*/Page\\b")) == 1);
    const QRegularExpressionMatch box =
        QRegularExpression("/MediaBox\\s*\\[\\s*0\\s+0\\s+([\\d.]+)\\s+([\\d.]+)").match(text);
    REQUIRE(box.hasMatch());
    CHECK(box.captured(1).toDouble() == Catch::Approx(1190.55).margin(1)); // 420 mm in points
    CHECK(box.captured(2).toDouble() == Catch::Approx(841.89).margin(1));  // 297 mm

    const QString svg = QString::fromStdString((dir / "plan.svg").string());
    REQUIRE(s.exportDrawing("plan", QUrl::fromLocalFile(svg)).isEmpty());
    QFile g(svg);
    REQUIRE(g.open(QIODevice::ReadOnly));
    const QByteArray xml = g.readAll();
    CHECK(xml.contains("<svg"));
    CHECK(xml.contains("viewBox=\"0 0 420 297\""));
    CHECK(s.exportDrawing("plan", QUrl::fromLocalFile("/nonexistent/dir/x.pdf")).size() > 0);
}

TEST_CASE("Session drawings: notes and sheet options are one undo step each; SVG carries notes, labels optional "
          "(FR-19)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.setDrawing(
        "plan", "notes",
        QVariantList{QVariantMap{{"at", QVariantList{1, 2}}, {"text", "Rigging point A"}, {"to", QVariantList{0, 0}}}});
    s.setDrawing("plan", "autoDimensions", false);
    CHECK(s.drawing_sheet("plan").notes.size() == 1);
    CHECK_FALSE(s.drawing_sheet("plan").auto_dimensions);
    s.undo();
    CHECK(s.drawing_sheet("plan").auto_dimensions);
    CHECK(s.drawing_sheet("plan").notes.size() == 1);
    s.undo();
    CHECK(s.drawing_sheet("plan").notes.empty());
    s.redo();

    const auto svg_text = [&](const char *name) {
        const QString svg = QString::fromStdString((dir / name).string());
        REQUIRE(s.exportDrawing("plan", QUrl::fromLocalFile(svg)).isEmpty());
        QFile f(svg);
        REQUIRE(f.open(QIODevice::ReadOnly));
        return f.readAll();
    };
    // The demo's first fixture ID as the painter letters it, as SVG text.
    int first_id = 0;
    for (const json &o : s.document().data()["objects"])
        if (o["type"] == "fixture" && o["fixtureId"].is_number()) {
            first_id = o["fixtureId"].get<int>();
            break;
        }
    REQUIRE(first_id > 0);
    const QByteArray id_text = ">" + QByteArray::number(first_id) + "<";
    const QByteArray with_labels = svg_text("notes.svg");
    CHECK(with_labels.contains("Rigging point A"));
    CHECK(with_labels.contains(id_text));
    s.setDrawing("plan", "labels", false);
    const QByteArray without = svg_text("nolabels.svg");
    CHECK(without.contains("Rigging point A"));
    CHECK_FALSE(without.contains(id_text));

    // MCP: the same keys, one AI undo step; export is gated by file access.
    const auto tools = make_mcp_tools(s);
    const json notes = json::array({{{"at", {0, 1}}, {"text", "Desk"}}});
    const json after = tool(tools, "set_drawing", {{"kind", "plan"}, {"key", "notes"}, {"value", notes}});
    CHECK(after["notes"] == notes);
    CHECK(s.document().history()[s.document().cursor() - 1].label.starts_with("AI: "));
    CHECK(tool(tools, "get_drawing", {{"kind", "plan"}})["labels"] == false);
    const auto exporter =
        std::find_if(tools.begin(), tools.end(), [](const auto &t) { return t.name == "export_drawing"; });
    REQUIRE(exporter != tools.end());
    CHECK_FALSE(exporter->available());
}

TEST_CASE("NDI loopback: a local sender is found, received as BGRA and followed through a size change (FR-13)",
          "[ndi]") {
    const NDIlib_v6 *ndi = sv::media::ndi_api();
    if (!ndi)
        SKIP("NDI runtime not found; set NDI_RUNTIME_DIR_V6");
    NDIlib_send_create_t settings;
    settings.p_ndi_name = "sv-loopback";
    settings.clock_video = false;
    NDIlib_send_instance_t sender = ndi->send_create(&settings);
    REQUIRE(sender);
    const auto send = [&](int w, int h, quint32 bgra) {
        std::vector<quint32> pixels(size_t(w) * size_t(h), bgra);
        NDIlib_video_frame_v2_t frame;
        frame.xres = w;
        frame.yres = h;
        frame.FourCC = NDIlib_FourCC_video_type_BGRA;
        frame.frame_format_type = NDIlib_frame_format_type_progressive;
        frame.p_data = reinterpret_cast<uint8_t *>(pixels.data());
        frame.line_stride_in_bytes = w * 4;
        ndi->send_send_video_v2(sender, &frame);
    };
    // Waits up to `ms`, sending frames meanwhile, until `done` holds; pumps events like the app's UI thread.
    const auto spin = [&](int ms, int w, int h, quint32 bgra, auto done) {
        QElapsedTimer timer;
        timer.start();
        for (; timer.elapsed() < ms; QThread::msleep(20)) {
            send(w, h, bgra);
            QCoreApplication::processEvents();
            if (done())
                return true;
        }
        return false;
    };

    QString name;
    for (int tries = 0; tries < 10 && name.isEmpty(); ++tries)
        for (const QString &source : sv::media::ndi_sources(500))
            if (source.endsWith("(sv-loopback)"))
                name = source;
    REQUIRE_FALSE(name.isEmpty());
    CHECK(sv::media::MediaSource::available("ndi").contains(name));

    sv::media::MediaSource media;
    media.open("ndi", name);
    REQUIRE(media.active());
    quint64 serial = 0;
    QImage got;
    const auto frame_of = [&](QSize size) {
        return [&, size] {
            if (QImage image = media.latest(serial); !image.isNull())
                got = image;
            return got.size() == size;
        };
    };
    REQUIRE(spin(5000, 320, 180, 0xff2040c0u, frame_of({320, 180})));
    // BGRA in memory is ARGB32's 0xAARRGGBB; NDI compresses even on loopback, so channels come back near, not equal.
    const QColor c = got.pixelColor(10, 10);
    CHECK(std::abs(c.red() - 0x20) < 12);
    CHECK(std::abs(c.green() - 0x40) < 12);
    CHECK(std::abs(c.blue() - 0xc0) < 12);
    REQUIRE(spin(5000, 640, 360, 0xff00ff00u, frame_of({640, 360})));
    const sv::media::MediaStatus status = media.status();
    CHECK(status.signal);
    CHECK(status.fps > 0);
    CHECK(status.latency_ms >= 0);

    // Signal output: an NdiSender relays a source's frames under its own name.
    {
        sv::media::NdiSender relay("sv-relay");
        relay.set_source(&media);
        QString relayed;
        QElapsedTimer found; // discovery answers at once with what it knows, so wait on the clock
        for (found.start(); found.elapsed() < 8000 && relayed.isEmpty(); QThread::msleep(200))
            for (const QString &source : sv::media::ndi_sources(500))
                if (source.endsWith("(sv-relay)"))
                    relayed = source;
        REQUIRE_FALSE(relayed.isEmpty());
        sv::media::MediaSource back;
        back.open("ndi", relayed);
        quint64 back_serial = 0;
        QImage back_frame;
        REQUIRE(spin(5000, 640, 360, 0xff00ff00u, [&] {
            if (QImage image = back.latest(back_serial); !image.isNull())
                back_frame = image;
            return back_frame.size() == QSize(640, 360);
        }));
        CHECK(back_frame.pixelColor(10, 10).green() > 230);
        CHECK(relay.fps() >= 0);
        back.open({}, {});
    }

    media.open({}, {}); // joins the receiver thread before the sender goes away
    ndi->send_destroy(sender);
}

TEST_CASE("Session keeps ten recent projects, newest first, dropping files that are gone") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests" / "recent";
    std::filesystem::create_directories(dir);
    const auto url = [&](int i) {
        return QUrl::fromLocalFile(QString::fromStdString((dir / "p").string()) + QString::number(i) + ".svproj");
    };
    QSettings("SceneVisualiser", "Scene Visualiser").remove("recentFiles");
    Session s(nullptr);
    s.set_project(sv::new_project());
    for (int i = 0; i < 12; ++i)
        REQUIRE(s.saveProject(url(i)).isEmpty());
    QVariantList recent = s.recent_files();
    REQUIRE(recent.size() == 10);
    CHECK(recent.front().toMap()["url"].toUrl() == url(11));
    CHECK(recent.front().toMap()["name"] == "p11");

    REQUIRE(s.openProject(url(5)).isEmpty()); // opening moves it to the top without a duplicate
    recent = s.recent_files();
    CHECK(recent.size() == 10);
    CHECK(recent.front().toMap()["url"].toUrl() == url(5));

    std::filesystem::remove(url(11).toLocalFile().toStdString());
    recent = s.recent_files();
    CHECK(recent.size() == 9);
    CHECK(std::ranges::none_of(recent, [&](const QVariant &r) { return r.toMap()["url"].toUrl() == url(11); }));
}

TEST_CASE("Session lists missing types, geometry and media, and packs linked media into the project") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const QString png = QString::fromStdString((dir / "linked.png").string());
    QImage(4, 4, QImage::Format_RGB32).save(png);

    json p = sv::new_project();
    p["objects"].push_back(sv::new_fixture("Lost", "Nobody@Nothing.gdtf", "Default"));
    p["objects"].push_back(sv::new_set_piece("Gone", "geometry/gone.obj"));
    p["objects"].push_back(sv::new_fixture("Here", "Generic@Spot_7ch.gdtf", "Default"));
    p["composition"]["source"] = {{"kind", "still"}, {"file", png.toStdString()}, {"name", "linked.png"}};
    Session s(nullptr);
    s.set_project(p);
    CHECK(s.missing_files() == QStringList{"Fixture type Nobody@Nothing.gdtf", "Geometry geometry/gone.obj"});
    CHECK(s.linked_media() == QStringList{png});

    REQUIRE(s.packMedia().isEmpty());
    CHECK(s.linked_media().isEmpty());
    CHECK(s.document().data()["composition"]["source"]["file"] == "media/linked.png");
    CHECK(s.dirty());
    CHECK_FALSE(s.packMedia().isEmpty()); // nothing left to pack

    const auto path = dir / "packed.svproj";
    REQUIRE(s.saveProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(sv::load_package(path).files.contains("media/linked.png"));

    s.edit("composition", "/source/file", png + ".gone");
    CHECK(s.missing_files().contains("Media " + png + ".gone"));
}

TEST_CASE("Session \"Don't save\" deletes the autosave, so discarded edits are not offered back (AT-6)") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / "discard.svproj";
    const QUrl url = QUrl::fromLocalFile(QString::fromStdString(path.string()));
    Session s(nullptr);
    s.set_project(sv::demo_project());
    REQUIRE(s.saveProject(url).isEmpty());
    s.edit(QString::fromStdString(s.document().data()["objects"][0]["uuid"]), "/name", "Thrown away");
    s.autosave();
    REQUIRE(std::filesystem::exists(sv::autosave_path(path)));
    s.discardChanges();
    CHECK_FALSE(std::filesystem::exists(sv::autosave_path(path)));
    Session t(nullptr);
    REQUIRE(t.openProject(url).isEmpty());
    CHECK(t.recovery_file().isEmpty());
}

TEST_CASE("FR-4 saveFixtureType adds a usable type, refuses bundled names, and the package carries it unused") {
    Session s(nullptr);
    s.set_project(sv::new_project());
    const QVariantMap bundled = s.fixtureSpec("Generic@Spot_7ch.gdtf");
    CHECK(bundled["bundled"].toBool());
    CHECK(bundled["warnings"].toStringList().isEmpty());
    const QStringList pixel = s.fixtureSpec("Generic@Pixel_Bar_8.gdtf")["warnings"].toStringList();
    CHECK(pixel.join(' ').contains("merged")); // 8 cells of RGB become one RGB
    CHECK(bundled["channels"].toList().size() >= 5);

    QVariantMap spec = bundled;
    CHECK(s.saveFixtureType(spec) == "Bundled types are read-only; change the name");
    spec["name"] = "My Spot";
    spec["manufacturer"] = "Acme";
    spec["emitter"] = "#ff8000";
    QVariantList channels{QVariantMap{{"attribute", "Dimmer"}, {"bits", 8}},
                          QVariantMap{{"attribute", "Pan"}, {"bits", 16}, {"default", 32768}},
                          QVariantMap{{"attribute", "Color1"}, {"bits", 8}}};
    spec["channels"] = channels;
    REQUIRE(s.saveFixtureType(spec).isEmpty());
    CHECK(s.fixture_types().contains("Acme@My_Spot.gdtf"));
    CHECK(s.modes("Acme@My_Spot.gdtf") == QStringList{"Default"});

    const QVariantMap back = s.fixtureSpec("Acme@My_Spot.gdtf");
    CHECK_FALSE(back["bundled"].toBool());
    CHECK(back["name"] == "My Spot");
    CHECK(back["emitter"] == "#ff8000");
    const QVariantList ch = back["channels"].toList();
    REQUIRE(ch.size() == 3);
    CHECK(ch[1].toMap()["bits"] == 16);
    CHECK(ch[1].toMap()["default"] == 32768);
    CHECK(ch[1].toMap()["physicalFrom"] == -270);
    CHECK(s.addFixtureArray("Acme@My_Spot.gdtf", "Default", 2, {0.0, 4.0, 0.0}, {1.0, 0.0, 0.0}, "1.1", "").isEmpty());
    const json &f = s.document().data()["objects"].back();
    CHECK(f["patch"] == json{{"universe", 1}, {"address", 5}}); // 1 + 2 + 1 channels after 1.001

    s.deleteSelection(); // unused types still travel with the project
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / "built.svproj";
    REQUIRE(s.saveProject(QUrl::fromLocalFile(QString::fromStdString(path.string()))).isEmpty());
    CHECK(sv::load_package(path).files.contains("gdtf/Acme@My_Spot.gdtf"));
    CHECK_FALSE(sv::load_package(path).files.contains("gdtf/Generic@Spot_7ch.gdtf"));

    CHECK_FALSE(s.saveFixtureType(QVariantMap{{"name", "X"}, {"manufacturer", "Y"}}).isEmpty()); // no channels
    spec["channels"] = QVariantList{QVariantMap{{"attribute", "Dimmer"}}, QVariantMap{{"attribute", "Dimmer"}}};
    CHECK_FALSE(s.saveFixtureType(spec).isEmpty());
}

TEST_CASE("Generator mapping: fixtures take the composition colour under the nearest LED wall (FR-11)") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.start_dmx();
    const auto wait = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    const sv::json &project = s.document().data();
    const auto probes = sv::render::mapping_probes(project, *s.library_snapshot());
    REQUIRE(probes.size() == sv::render::generator_targets(project, *s.library_snapshot()).size());
    REQUIRE_FALSE(probes.empty());
    bool inside_a_wall = false;
    for (const sv::json &p : probes) {
        REQUIRE(p.is_object()); // the demo rig has walls
        for (const sv::json &o : project.at("objects"))
            if (o.value("type", std::string()) == "ledWall") {
                const sv::json &sl = o.at("slice");
                inside_a_wall =
                    inside_a_wall || (p["x"] >= sl["x"] && p["x"] <= sl["x"].get<double>() + sl["w"].get<double>() &&
                                      p["y"] >= sl["y"] && p["y"] <= sl["y"].get<double>() + sl["h"].get<double>());
            }
    }
    CHECK(inside_a_wall);

    // An LED string right beside a fixture beats the flat walls: the probe lands in the string's slice column,
    // at the run along the string.
    {
        sv::json p2 = s.document().data();
        sv::json fixture;
        for (const sv::json &o : p2.at("objects"))
            if (o.value("type", std::string()) == "fixture" && o.value("patch", sv::json()).is_object()) {
                fixture = o;
                break;
            }
        REQUIRE(fixture.is_object());
        const sv::json pos = fixture.at("position");
        sv::json net = sv::new_led_wall("Net");
        net["position"] = {pos[0].get<double>() + 0.2, pos[1].get<double>() - 1.0, pos[2].get<double>()};
        net["strings"] =
            sv::json::array({sv::json::array({{0, 0, 0}, {0, 2, 0}}), sv::json::array({{1, 0, 0}, {1, 2, 0}})});
        net["slice"] = {{"x", 1000}, {"y", 100}, {"w", 200}, {"h", 400}};
        p2["objects"].push_back(net);
        std::size_t index = 0;
        for (const sv::json &o : p2.at("objects")) {
            if (o == fixture)
                break;
            if (o.value("type", std::string()) == "fixture" && o.value("patch", sv::json()).is_object())
                ++index;
        }
        const auto probes2 = sv::render::mapping_probes(p2, *s.library_snapshot());
        REQUIRE(probes2.size() > index);
        const sv::json &pr = probes2[index];
        CHECK(pr["x"].get<double>() == Catch::Approx(1050));          // first of two columns
        CHECK(pr["y"].get<double>() == Catch::Approx(300).margin(1)); // half way up the string
    }

    const int u = s.universes().at(0).toInt();
    const auto lit = [&] {
        for (const auto &st : s.dmx().status())
            if (st.universe == u)
                return std::ranges::any_of(st.data, [](auto v) { return v != 0; });
        return false;
    };
    s.setGenerator("bogus");
    CHECK(s.generator() == "off");
    Session::setPreference("generatorSmoothMs", 0);
    Session::setPreference("generatorMax", 0.5); // the ceiling: no mapped slot above 128
    s.setGenerator("mapping");
    CHECK(s.generator() == "mapping");
    wait(1000); // the pixel thread samples the test pattern, the engine ticks at 44 Hz
    CHECK(lit());
    for (const auto &st : s.dmx().status())
        if (st.universe == u)
            CHECK(std::ranges::max(st.data) <= 128);
    Session::setPreference("generatorMax", 1.0);
    s.setGenerator("off");
    wait(200);
    CHECK_FALSE(lit());
}

TEST_CASE("Show recording captures the merged DMX and plays it back over live input, also through MCP") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.start_dmx();
    const auto dir = std::filesystem::temp_directory_path() / "sv_show_recording";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const QString base = QString::fromStdString((dir / "show").string());
    const auto wait = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    const int u = s.universes().at(0).toInt();
    const auto universe_status = [&] {
        for (const auto &st : s.dmx().status())
            if (st.universe == u)
                return st;
        FAIL("universe " << u << " not patched");
        throw;
    };
    const auto lit = [&] { return std::ranges::any_of(universe_status().data, [](auto v) { return v != 0; }); };

    s.set_generator(sv::proto::Pattern::full);
    REQUIRE(s.startRecording(QUrl::fromLocalFile(base), false).isEmpty());
    CHECK(s.recording()["active"].toBool());
    CHECK(s.startPlayback(QUrl::fromLocalFile(base + ".svdmx")) == "Stop recording before playing back");
    wait(1500);
    s.stopRecording();
    s.set_generator(sv::proto::Pattern::off);
    CHECK_FALSE(s.recording()["active"].toBool());
    CHECK(std::filesystem::exists(dir / "show.svdmx"));
    CHECK_FALSE(std::filesystem::exists(dir / "show.mp4")); // no live media source
    wait(200);
    CHECK_FALSE(lit());

    REQUIRE(s.startPlayback(QUrl::fromLocalFile(base + ".svdmx")).isEmpty());
    CHECK(s.recording()["duration"].toLongLong() > 1000);
    CHECK_FALSE(s.recording()["hasVideo"].toBool());
    wait(200);
    CHECK(lit());
    CHECK(universe_status().sources == std::vector<std::string>{"Playback"});
    s.stopPlayback();
    wait(200);
    CHECK_FALSE(lit());

    // The same through MCP, merging instead of replacing; record needs file access.
    const auto tools = make_mcp_tools(s);
    const auto play = std::ranges::find(tools, std::string("play"), &sv::mcp::Tool::name);
    REQUIRE(play != tools.end());
    const json state =
        tool(tools, "play", {{"path", (dir / "show.svdmx").string()}, {"replace", false}, {"seek", 500}});
    CHECK(state["playing"] == true);
    CHECK(state["replace"] == false);
    wait(200);
    CHECK(lit());
    CHECK(tool(tools, "get_recording")["playing"] == true);
    json scoped = tool(tools, "play", {{"scopes", {{"default", "live"}}}, {"fade", 0.5}});
    CHECK(scoped["fade"] == 0.5);
    CHECK(scoped["scopes"]["default"] == "live");
    wait(200);
    CHECK_FALSE(lit()); // everything live, and live is dark
    scoped = tool(tools, "play", {{"scopes", {{"default", {{"mode", "recording"}, {"fade", 0}}}}}, {"fade", 1}});
    CHECK(scoped["fades"]["default"] == 0);
    wait(200);
    CHECK_FALSE(lit()); // the part's own fader is down
    tool(tools, "play", {{"scopes", {{"default", {{"mode", "recording"}, {"fade", nullptr}}}}}, {"fade", 1}});
    wait(200);
    CHECK(lit());

    // Proxy: what the view shows goes out; status reports it under get_recording.output.
    json out = tool(tools, "set_output", {{"enabled", true}, {"target", "127.0.0.1"}, {"offset", 100}});
    CHECK(out["enabled"] == true);
    CHECK(out["offset"] == 100);
    wait(1200);
    CHECK(tool(tools, "get_recording")["output"]["universes"].get<int>() > 0);
    CHECK(tool(tools, "set_output", {{"enabled", false}})["enabled"] == false);
    CHECK(tool(tools, "play", {{"stop", true}})["playing"] == false);
    CHECK_THROWS_WITH(tool(tools, "play", {{"path", (dir / "missing.svdmx").string()}}), "Cannot open missing.svdmx");

    // With a live media source the picture goes to an .mp4 beside it (an H.264 encoder must exist).
    REQUIRE(s.setMediaFile("video", QUrl::fromLocalFile(SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4")).isEmpty());
    wait(500);
    REQUIRE(s.startRecording(QUrl::fromLocalFile(base + "2.svdmx"), false).isEmpty());
    CHECK(s.recording()["hasVideo"].toBool());
    wait(1500);
    s.stopRecording();
    wait(500);
    INFO(s.recording()["error"].toString().toStdString());
    CHECK(std::filesystem::exists(dir / "show2.mp4"));
    CHECK(sv::DmxRecording::load(dir / "show2.svdmx").state_at(0).contains(std::uint16_t(u))); // seeded at t = 0

    REQUIRE(s.startPlayback(QUrl::fromLocalFile(base + "2.svdmx")).isEmpty());
    CHECK(s.recording()["hasVideo"].toBool());
    s.stopPlayback();
    CHECK(s.media().active()); // the project's own source again
}

TEST_CASE("Playback chases Art-Net timecode minus the offset, holds without it and never loops") {
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    {
        sv::DmxRecordWriter w(dir / "chase.svdmx");
        sv::DmxSlots slots{};
        for (std::uint32_t t : {0u, 400u, 1000u}) {
            slots[0] = std::uint8_t(t / 5);
            w.write(t, 1, slots);
        }
        w.close();
    }
    Session s(nullptr);
    s.set_project(sv::new_project());
    CHECK_FALSE(s.setPlaybackChase(true, "1:00").isEmpty()); // neither HH:MM:SS nor HH:MM:SS:FF
    REQUIRE(s.startPlayback(QUrl::fromLocalFile(QString::fromStdString((dir / "chase.svdmx").string()))).isEmpty());
    REQUIRE(s.setPlaybackChase(true, "01:00:00:00").isEmpty());
    CHECK(s.recording()["chase"].toBool());
    s.chase_timecode(3'600'400);
    CHECK(s.recording()["t"].toLongLong() == 400);
    s.chase_timecode(-1); // timecode stopped: hold
    CHECK(s.recording()["t"].toLongLong() == 400);
    s.chase_timecode(3'599'000); // before the offset: the start
    CHECK(s.recording()["t"].toLongLong() == 0);
    s.chase_timecode(3'700'000); // past the end: the last frame, no loop
    CHECK(s.recording()["t"].toLongLong() == 1000);
    REQUIRE(s.setPlaybackChase(false, "").isEmpty());
    CHECK(s.recording()["chaseOffset"].toLongLong() == 3'600'000);
    CHECK(s.recording()["t"].toLongLong() >= 1000); // carries on from the timecode's place
    s.stopPlayback();
}

TEST_CASE("Playback scopes: fixture over group over output over default; dimmers fade, wheels snap") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const json &objects = s.document().data().at("objects");
    const auto find = [&](const std::string &gdtf, int nth = 0) -> const json & {
        for (const json &o : objects)
            if (o.value("gdtf", std::string()) == gdtf && nth-- == 0)
                return o;
        FAIL("no " << gdtf);
        throw;
    };
    const auto slot = [&](const json &o, const std::string &attr) {
        const auto *mode = s.library().at(o["gdtf"].get<std::string>()).mode(o["mode"].get<std::string>());
        for (const auto &ch : mode->channels)
            if (ch.attribute() == attr)
                return std::pair{std::uint16_t(o["patch"]["universe"]), int(o["patch"]["address"]) - 1 + ch.offsets[0]};
        FAIL(attr);
        throw;
    };
    const auto weight = [&](std::pair<std::uint16_t, int> at) {
        return int(s.playback_mix().at(at.first).weight[at.second]);
    };

    const json &beam = find("Generic@Beam_5ch.gdtf"), &beam2 = find("Generic@Beam_5ch.gdtf", 1);
    const json &wash = find("Generic@Wash_RGBW.gdtf");
    const auto dimmer = slot(beam, "Dimmer"), wheel = slot(beam, "Color1"), gobo = slot(beam, "Gobo1");
    const auto &mix = s.playback_mix().at(dimmer.first);
    CHECK_FALSE(mix.snap[dimmer.second]);
    CHECK(mix.snap[wheel.second]);
    CHECK(mix.snap[gobo.second]);
    CHECK(mix.snap[511]); // unpatched slots switch
    CHECK(weight(dimmer) == 255);

    QString beams;
    for (const QVariant &g : s.control_groups())
        if (g.toMap()["kind"] == "type" && g.toMap()["name"] == "Beam 5ch")
            beams = g.toMap()["key"].toString();
    REQUIRE_FALSE(beams.isEmpty());
    s.setPlaybackScope(beams, "live");
    CHECK(weight(dimmer) == 0);
    CHECK(weight(slot(beam2, "Dimmer")) == 0);
    CHECK(weight(slot(wash, "Dimmer")) == 255);
    s.setPlaybackScope(QString::fromStdString(beam["uuid"]), "both"); // the fixture beats its group
    CHECK(weight(dimmer) == 255);
    CHECK(s.playback_mix().at(dimmer.first).htp[dimmer.second]);
    CHECK(weight(slot(beam2, "Dimmer")) == 0);
    s.setPlaybackScope("default", "live");
    CHECK(weight(slot(wash, "Dimmer")) == 0);
    s.setPlaybackScope("default", ""); // the default stays
    CHECK(s.recording()["scopes"].toMap()["default"] == "live");
    s.setPlaybackScope(QString::fromStdString(beam["uuid"]), "");
    CHECK(weight(dimmer) == 0);
    s.setPlaybackFade(2);
    CHECK(s.recording()["fade"].toDouble() == 1);

    // Each part's own fader, resolved like the modes but independently of them
    s.setPlaybackScope("default", "recording");
    s.setPlaybackScope(beams, "");
    s.setPlaybackScopeFade(beams, 0.5);
    CHECK(weight(dimmer) == 128);
    CHECK(weight(slot(beam2, "Dimmer")) == 128);
    CHECK(weight(slot(wash, "Dimmer")) == 255);
    s.setPlaybackScopeFade(QString::fromStdString(beam["uuid"]), 0.2); // the fixture beats its group
    CHECK(weight(dimmer) == 51);
    CHECK(weight(slot(beam2, "Dimmer")) == 128);
    s.setPlaybackScope(beams, "live"); // a Live mode still wins over any fader
    CHECK(weight(dimmer) == 0);
    s.setPlaybackScope(beams, "");
    s.setPlaybackScopeFade("default", 0.4);
    CHECK(weight(slot(wash, "Dimmer")) == 102);
    CHECK(weight(slot(beam2, "Dimmer")) == 128);
    s.setPlaybackScopeFade(QString::fromStdString(beam["uuid"]), -1); // dropped: back to the group's
    CHECK(weight(dimmer) == 128);
    CHECK(s.recording()["fades"].toMap()[beams].toDouble() == 0.5);

    s.set_project(sv::demo_project()); // live state, cleared on open
    CHECK(s.recording()["scopes"].toMap().size() == 1);
    CHECK(s.recording()["fades"].toMap().isEmpty());
}

TEST_CASE("Locate holds fixtures white at full and gives back what was held before") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    QStringList fixtures;
    for (const json &o : s.document().data().at("objects"))
        if (o["type"] == "fixture" && fixtures.size() < 2)
            fixtures << QString::fromStdString(o["uuid"].get<std::string>());
    REQUIRE(fixtures.size() == 2);
    s.setLight({fixtures[0]}, {{"Pan", 0.3}, {"Dimmer", 0.2}});
    s.locate(fixtures, true);
    CHECK(s.located().size() == 2);
    auto held = s.programmer()[fixtures[0]].toMap();
    CHECK(held["Dimmer"].toDouble() == 1);
    CHECK(held["ColorAdd_R"].toDouble() == 1);
    s.locate(fixtures, false);
    CHECK(s.located().isEmpty());
    held = s.programmer()[fixtures[0]].toMap();
    CHECK(held["Pan"].toDouble() == Catch::Approx(0.3));
    CHECK(held["Dimmer"].toDouble() == Catch::Approx(0.2)); // restored, not dropped
    CHECK_FALSE(held.contains("ColorAdd_R"));
    CHECK_FALSE(s.programmer().contains(fixtures[1])); // nothing held before: released
}

TEST_CASE("Looks store the programmer in the project and recall it, skipping fixtures deleted since") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    QStringList fixtures;
    for (const json &o : s.document().data().at("objects"))
        if (o["type"] == "fixture" && fixtures.size() < 2)
            fixtures << QString::fromStdString(o["uuid"].get<std::string>());
    REQUIRE(fixtures.size() == 2);
    s.setLight({fixtures[0]}, {{"Pan", 0.3}});
    s.setLight({fixtures[1]}, {{"Dimmer", 0.7}});
    const QVariantMap snapshot = s.programmer();
    CHECK_FALSE(s.saveLook("  ").isEmpty());
    REQUIRE(s.saveLook("Warm").isEmpty());
    REQUIRE(s.looks().size() == 1);
    const QString look = s.looks()[0].toMap()["uuid"].toString();
    s.releaseLights({});
    CHECK(s.programmer().isEmpty());
    s.recallLook(look);
    CHECK(s.programmer() == snapshot);

    s.releaseLights({});
    s.selectUuids({fixtures[1]});
    s.deleteSelection();
    s.recallLook(look);
    CHECK(s.programmer().keys() == QStringList{fixtures[0]});
    s.deleteLook(look);
    CHECK(s.looks().isEmpty());
}

TEST_CASE("setLight on one head holds it under heads; the fixture-wide dimmer stays whole") {
    const std::string xml = R"(<GDTF><FixtureType Name="Twin" Manufacturer="Test"><Geometries><Geometry Name="Base">
        <Axis Name="Yoke 1"><Axis Name="Head 1"><GeometryReference Name="Px 1" Geometry="Pixel">
          <Break DMXBreak="1" DMXOffset="4"/></GeometryReference></Axis></Axis>
        <Axis Name="Yoke 2"><Axis Name="Head 2"><GeometryReference Name="Px 2" Geometry="Pixel">
          <Break DMXBreak="1" DMXOffset="6"/></GeometryReference></Axis></Axis>
        </Geometry><Geometry Name="Pixel"><Beam Name="LED"/></Geometry></Geometries>
        <DMXModes><DMXMode Name="m" Geometry="Base"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Base"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Yoke 1"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="Yoke 2"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="1" Geometry="Pixel" DMXBreak="1"><LogicalChannel><ChannelFunction Attribute="ColorAdd_R"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Pixel" DMXBreak="1"><LogicalChannel><ChannelFunction Attribute="ColorAdd_B"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)";
    const auto dir = std::filesystem::temp_directory_path() / "sv_session_tests";
    std::filesystem::create_directories(dir);
    const auto zip = sv::write_zip({{"description.xml", std::vector<std::uint8_t>(xml.begin(), xml.end())}});
    std::ofstream(dir / "Test@Twin.gdtf", std::ios::binary)
        .write(reinterpret_cast<const char *>(zip.data()), std::streamsize(zip.size()));
    Session s(nullptr);
    s.set_project(sv::new_project());
    REQUIRE(s.importGdtf(QUrl::fromLocalFile(QString::fromStdString((dir / "Test@Twin.gdtf").string()))).isEmpty());
    REQUIRE(s.addObject(
                 "fixture",
                 {{"gdtf", "Test@Twin.gdtf"}, {"mode", "m"}, {"patch", QVariantMap{{"universe", 1}, {"address", 1}}}})
                .isEmpty());
    const QString fx = selected(s);
    CHECK(s.controlHeads({fx}) == 2);

    s.setLight({fx}, {{"ColorAdd_R", 1.0}, {"Dimmer", 1.0}}, 1);
    s.setLight({fx}, {{"ColorAdd_B", 1.0}}, 0);
    const json held = sv::json::parse(QJsonDocument::fromVariant(s.programmer()[fx]).toJson().toStdString());
    CHECK(held["Dimmer"] == 1.0); // no Dimmer channel on a head: fixture-wide
    CHECK(held["heads"]["1"] == json{{"ColorAdd_R", 1.0}});
    CHECK(held["heads"]["0"] == json{{"ColorAdd_B", 1.0}});

    // What push_programmer sends decodes to a blue head 0 and a red head 1.
    const auto &type = s.library().at("Test@Twin.gdtf");
    std::array<std::uint8_t, 512> frame{};
    for (const auto &[attrs, head] :
         {std::pair{json{{"Dimmer", 1.0}}, -1}, {held["heads"]["0"], 0}, {held["heads"]["1"], 1}})
        for (const auto &b : s.encode_fixture(fx.toStdString(), attrs, head, false).bytes)
            frame[b.offset] = b.value;
    const auto a = sv::gdtf::decode(type, *type.mode("m"), frame, 1);
    REQUIRE(a.heads.size() == 2);
    CHECK(a.heads[0].colour[2] > 0.9f);
    CHECK(a.heads[0].colour[0] < 0.1f);
    CHECK(a.heads[1].colour[0] > 0.9f);
    CHECK(a.heads[1].colour[2] < 0.1f);

    s.setLight({fx}, {{"ColorAdd_B", QVariant()}}, 0); // a null drops it, and the empty head goes
    CHECK_FALSE(s.programmer()[fx].toMap()["heads"].toMap().contains("0"));
}

TEST_CASE("editSelection sets a field on the selection in one undo step, filtered by type; fitVenue") {
    Session s(nullptr);
    s.set_project(sv::new_project());
    REQUIRE(s.addObject("shape", {}).isEmpty());
    const QString a = selected(s);
    REQUIRE(s.addObject("shape", {}).isEmpty());
    const QString b = selected(s);
    REQUIRE(
        s.addObject("deck", {{"position", QVariantList{9.0, 0.0, 0.0}}, {"width", 4.0}, {"height", 1.0}}).isEmpty());
    const QString d = selected(s);
    s.selectUuids({a, b, d});
    const auto steps = s.document().history().size();
    CHECK(s.editSelection("/color", "#112233", "shape").isEmpty());
    CHECK(s.document().history().size() == steps + 1);
    CHECK(object(s, a)["color"] == "#112233");
    CHECK(object(s, b)["color"] == "#112233");
    CHECK_FALSE(object(s, d).contains("color"));
    CHECK(s.editSelection("/layer", "L1").isEmpty());
    for (const QString &u : {a, b, d})
        CHECK(object(s, u)["layer"] == "L1");
    s.undo();
    CHECK(object(s, d).value("layer", json()) != "L1");

    CHECK(s.fitVenue().isEmpty()); // the deck reaches x = 11
    CHECK(s.document().data()["venue"]["width"] == 22.0);
    CHECK(s.document().history().back().label == "Fit venue");
}

TEST_CASE("Light control holds touched attributes over live input, by type, rig and saved group") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    s.start_dmx();
    const auto wait = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    const auto decoded = [&] {
        const auto status = s.dmx().status();
        return sv::render::decode_fixtures(s.document().data(), s.library(), [&](std::uint16_t u) {
            for (const auto &st : status)
                if (st.universe == u)
                    return std::span<const std::uint8_t>(st.data);
            return std::span<const std::uint8_t>();
        });
    };
    const auto group = [&](const QString &kind, const QString &name) {
        for (const QVariant &g : s.control_groups())
            if (g.toMap()["kind"] == kind && g.toMap()["name"] == name)
                return g.toMap()["uuids"].toStringList();
        FAIL("no " << kind.toStdString() << " group " << name.toStdString());
        throw;
    };

    // A rig group reaches fixtures through their parent chain; types group by GDTF.
    const json &objects = s.document().data().at("objects");
    std::vector<std::string> washes, spots;
    for (const json &o : objects)
        if (o.value("gdtf", std::string()).find("Wash") != std::string::npos)
            washes.push_back(o["uuid"]);
        else if (o.value("gdtf", std::string()).find("Spot") != std::string::npos)
            spots.push_back(o["uuid"]);
    REQUIRE(washes.size() >= 2);
    json patch = json::array({{{"op", "add"},
                               {"path", "/objects/-"},
                               {"value", {{"uuid", "g-truss"}, {"type", "group"}, {"name", "Back truss"}}}}});
    for (const std::string &w : washes) {
        const auto at = std::find_if(objects.begin(), objects.end(), [&](const json &o) { return o["uuid"] == w; });
        const auto i = at - objects.begin();
        patch.push_back({{"op", "add"}, {"path", "/objects/" + std::to_string(i) + "/parent"}, {"value", "g-truss"}});
    }
    REQUIRE(s.try_apply(patch, "group").empty());
    const QStringList truss = group("rig", "Back truss");
    CHECK(truss.size() == int(washes.size()));
    CHECK(group("type", "Spot 7ch").size() == int(spots.size()));

    // Held attributes win over the generator on their slots only, and colour reaches only fixtures that have it.
    s.set_generator(sv::proto::Pattern::off);
    s.setLight(truss, {{"Dimmer", 1.0}, {"ColorAdd_R", 1.0}, {"ColorAdd_G", 0.0}, {"ColorAdd_B", 0.0}});
    s.setLight(group("type", "Spot 7ch"), {{"Dimmer", 0.5}, {"ColorAdd_R", 1.0}});
    wait(200);
    auto st = decoded();
    CHECK(st.at(washes[0]).attrs.dimmer == Catch::Approx(1).margin(0.01));
    CHECK(st.at(washes[0]).attrs.colour[0] > 0.9f);
    CHECK(st.at(washes[0]).attrs.colour[1] < 0.1f);
    CHECK(st.at(spots[0]).attrs.dimmer == Catch::Approx(0.5).margin(0.01));
    CHECK(s.programmer().size() == int(washes.size() + spots.size()));
    CHECK(s.controlAttributes(truss).contains("ColorAdd_R"));
    CHECK_FALSE(s.controlAttributes(group("type", "Blinder")).contains("ColorAdd_R"));

    s.set_generator(sv::proto::Pattern::full); // live input: untouched attributes follow, held ones stay
    s.setLight(truss, {{"Dimmer", 0.25}});
    wait(300);
    st = decoded();
    CHECK(st.at(washes[0]).attrs.dimmer == Catch::Approx(0.25).margin(0.01));
    CHECK(st.at(spots[0]).attrs.dimmer == Catch::Approx(0.5).margin(0.01));
    s.setLight(truss, {{"Dimmer", QVariant()}}); // drop one attribute back to live
    wait(200);
    CHECK(decoded().at(washes[0]).attrs.dimmer == Catch::Approx(1).margin(0.01));
    s.releaseLights({});
    wait(200);
    CHECK(s.programmer().isEmpty());
    CHECK(decoded().at(spots[0]).attrs.dimmer == Catch::Approx(1).margin(0.01));
    s.set_generator(sv::proto::Pattern::off);

    // Saved groups live in the project, one undo step each.
    CHECK_FALSE(s.saveGroup(" ", truss).isEmpty());
    REQUIRE(s.saveGroup("Washes", truss).isEmpty());
    CHECK(group("saved", "Washes") == truss);
    s.undo();
    CHECK_FALSE(s.document().data().contains("groups"));
    s.redo();
    s.deleteGroup(QString::fromStdString(s.document().data()["groups"][0]["uuid"]));
    CHECK(s.document().data()["groups"].empty());
}

TEST_CASE("FR-13 each Resolume output takes its own feed; NDI senders match by stream name") {
    const QString arena = QStringLiteral(SV_SOURCE_DIR "/tests/fixtures/resolume/arena-7.17-futro-ajki26.xml");
    Session s(nullptr);
    s.set_project(sv::new_project());
    REQUIRE(s.importResolume(QUrl::fromLocalFile(arena)).isEmpty());
    const json outputs = s.document().data()["outputs"];
    std::string novastar, linsn;
    for (const json &o : outputs) {
        if (o["name"] == "NDI NOVASTAR")
            novastar = o["uuid"];
        if (o["name"] == "NDI LINSN TECHWALL ROMBY")
            linsn = o["uuid"];
    }
    REQUIRE_FALSE(novastar.empty());
    REQUIRE_FALSE(linsn.empty());

    const auto matches = Session::match_ndi_feeds(
        outputs, {"MEDIA-PC (NDI NOVASTAR)", "MEDIA-PC (Arena - Composition)", "OTHER (NDI LINSN TECHWALL ROMBY)"});
    CHECK(matches.size() == 2);
    CHECK(matches.at(novastar) == "MEDIA-PC (NDI NOVASTAR)");
    CHECK(matches.at(linsn) == "OTHER (NDI LINSN TECHWALL ROMBY)");
    CHECK(Session::match_ndi_feeds(outputs, {"MEDIA-PC (Something else)"}).empty());

    CHECK(s.feed_sources().empty());
    s.edit(QString::fromStdString(novastar), "/feed",
           QVariantMap{{"kind", "ndi"}, {"name", "MEDIA-PC (NDI NOVASTAR)"}});
    s.edit(QString::fromStdString(linsn), "/feed",
           QVariantMap{{"kind", "ndi"}, {"name", "x (NDI LINSN TECHWALL ROMBY)"}});
    CHECK(s.feed_sources().size() == 2);
    int fed = 0, lumiverse = 0;
    for (const QVariant &row : s.feed_status()) {
        const QVariantMap r = row.toMap();
        fed += r["feedKind"] == "ndi";
        lumiverse += r["kind"] == "lumiverse" && r["universe"].toInt() > 0;
    }
    CHECK(fed == 2);
    CHECK(lumiverse == 8);

    const auto before = s.feed_sources();
    s.undo();
    REQUIRE(s.feed_sources().size() == 1);
    CHECK(s.feed_sources()[0].first == novastar);
    s.edit(QString::fromStdString(linsn), "/feed", QVariantMap{{"kind", "ndi"}, {"name", "y"}});
    std::set<const sv::media::MediaSource *> pooled; // the closed source is reused, never deleted
    for (const auto &[uuid, source] : before)
        pooled.insert(source);
    for (const auto &[uuid, source] : s.feed_sources())
        CHECK(pooled.contains(source));
}

TEST_CASE("FR-5 view keys: numpad, emulation, presets and the card") {
    using sv::app::ViewAction, sv::app::find_view_key;
    for (bool blender : {true, false}) {
        REQUIRE(find_view_key(Qt::Key_4, Qt::NoModifier, true, blender, false));
        CHECK(find_view_key(Qt::Key_4, Qt::NoModifier, true, blender, false)->action == ViewAction::orbit);
        CHECK_FALSE(find_view_key(Qt::Key_4, Qt::NoModifier, false, blender, false));
        CHECK(find_view_key(Qt::Key_4, Qt::NoModifier, false, blender, true)->action == ViewAction::orbit);
        CHECK(find_view_key(Qt::Key_8, Qt::ControlModifier, true, blender, false)->action == ViewAction::pan);
        CHECK(find_view_key(Qt::Key_Equal, Qt::NoModifier, false, blender, true)->action == ViewAction::zoom);
        CHECK(find_view_key(Qt::Key_Minus, Qt::NoModifier, true, blender, false)->x < 0);
        CHECK_FALSE(find_view_key(Qt::Key_Left, Qt::AltModifier, false, blender, false)); // Alt+arrows nudge
        CHECK(find_view_key(Qt::Key_Left, Qt::NoModifier, true, blender, false)->action == ViewAction::orbit); // macOS
    }
    CHECK(find_view_key(Qt::Key_A, Qt::NoModifier, false, true, false)->action == ViewAction::select_all);
    CHECK(find_view_key(Qt::Key_A, Qt::NoModifier, false, false, false)->action == ViewAction::frame_all);
    CHECK(find_view_key(Qt::Key_R, Qt::NoModifier, false, true, false)->action == ViewAction::transform);
    CHECK(find_view_key(Qt::Key_R, Qt::NoModifier, false, false, false)->action == ViewAction::gizmo);

    // The card lists every group, in order.
    for (bool blender : {true, false}) {
        const QVariantList rows = sv::app::view_key_help(blender);
        REQUIRE(rows.size() > 20);
        QStringList groups;
        for (const QVariant &r : rows)
            if (groups.isEmpty() || groups.last() != r.toMap()["group"].toString())
                groups << r.toMap()["group"].toString();
        CHECK(groups == QStringList{"Camera", "View", "Fly", "Edit", "Modal"});
    }
}

TEST_CASE("Session builds DDP targets: DMX strips in RGB order, walls and slice strips from the composition") {
    Session s(nullptr);
    json p = sv::new_project();
    json strip = sv::new_pixel_strip("Strip", 2);
    strip["colorFormat"] = "GRB";
    strip["patch"] = {{"universe", 1}, {"address", 10}};
    strip["ddp"] = {{"enabled", true}, {"target", "10.0.0.5"}, {"port", 4048}, {"offset", 6}};
    json off = sv::new_pixel_strip("Off", 2);
    off["patch"] = {{"universe", 1}, {"address", 100}};
    off["ddp"] = {{"enabled", false}, {"target", "10.0.0.5"}};
    json wall = sv::new_led_wall("Wall");
    wall["ddp"] = {{"enabled", true}, {"target", "10.0.0.6"}};
    p["objects"] = json::array({strip, off, wall});
    s.set_project(p);

    const auto &targets = s.ddp_targets();
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].ip == "10.0.0.5");
    CHECK(targets[0].offset == 6);
    CHECK_FALSE(targets[0].rgbw);
    using Slot = std::pair<std::uint16_t, std::uint16_t>;
    CHECK(targets[0].channels == std::vector<Slot>{{1, 11}, {1, 10}, {1, 12}, {1, 14}, {1, 13}, {1, 15}});
    CHECK(targets[1].ip == "10.0.0.6");
    CHECK(targets[1].port == sv::proto::ddp_port);
    CHECK(targets[1].channels.empty()); // pixels come from the composition

    s.edit(QString::fromStdString(strip["uuid"]), "/ddp/enabled", false);
    CHECK(s.ddp_targets().size() == 1);
}

TEST_CASE("Session sends a DDP wall's composition pixels from its own thread, whole frames ending in PUSH") {
    Session s(nullptr);
    QUdpSocket rx;
    REQUIRE(rx.bind(QHostAddress::LocalHost, 0));
    s.start_dmx();
    json p = sv::new_project();
    json wall = sv::new_led_wall("Wall");
    wall["pixelPitch"] = 50.0; // 80 x 50 pixels: a default wall is 2 MB a frame, more than loopback keeps up with
    wall["ddp"] = {{"enabled", true}, {"target", "127.0.0.1"}, {"port", rx.localPort()}};
    p["objects"] = json::array({wall});
    s.set_project(p);
    const auto derived = sv::wall_derived(wall);
    const std::size_t frame = std::size_t(derived.pixels_x) * std::size_t(derived.pixels_y) * 3;

    std::size_t bytes = 0, pushes = 0, lit = 0;
    QElapsedTimer clock;
    clock.start();
    while (pushes < 2 && clock.elapsed() < 3000) {
        if (!rx.waitForReadyRead(100))
            continue;
        while (rx.hasPendingDatagrams()) {
            const QByteArray d = rx.receiveDatagram().data();
            REQUIRE(d.size() >= 10);
            if (pushes == 1) { // count the second whole frame only
                bytes += std::size_t(d.size() - 10);
                lit += std::size_t(std::ranges::count_if(d.mid(10), [](char c) { return c != 0; }));
            }
            pushes += d[0] & 1;
        }
    }
    CHECK(pushes == 2);
    CHECK(bytes == frame);
    CHECK(lit > 0); // the test pattern, not a black frame
}

TEST_CASE("Offline render: camera_at eases between keys, holds past both ends and holds missing fields") {
    CHECK(camera_at(sv::json::array(), 0).empty());
    const sv::json keys = {{{"t", 0}, {"yaw", 0}, {"target", {0, 0, 0}}, {"fov", 40}},
                           {{"t", 1000}, {"yaw", 90}, {"target", {2, 4, 6}}},
                           {{"t", 2000}, {"fov", 60}}};
    CHECK(camera_at(keys, -500)["yaw"].get<double>() == 0);
    CHECK_FALSE(camera_at(keys, 0).contains("t"));
    const sv::json mid = camera_at(keys, 500);
    CHECK(mid["yaw"].get<double>() == Catch::Approx(45));
    CHECK(mid["target"][1].get<double>() == Catch::Approx(2));
    CHECK(mid["fov"].get<double>() == Catch::Approx(40));
    CHECK(camera_at(keys, 250)["yaw"].get<double>() < 22.5); // eased, not linear
    const sv::json last = camera_at(keys, 1500);
    CHECK(last["yaw"].get<double>() == Catch::Approx(90)); // held from the second key
    CHECK(last["fov"].get<double>() == Catch::Approx(50));
    CHECK(camera_at(keys, 9000)["fov"].get<double>() == Catch::Approx(60));
    CHECK(camera_at(sv::json::array({{{"t", 500}, {"yaw", 10}}}), 0)["yaw"].get<double>() == 10);
}

TEST_CASE("MediaSource stepping hands out every frame in order (section 26)") {
    sv::media::MediaSource m;
    const auto wait_until = [](auto pred, int ms) {
        QElapsedTimer t;
        t.start();
        while (!pred() && t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return pred();
    };
    m.open("video", SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4");
    if (!wait_until([&] { return m.duration_ms() > 0; }, 5000)) {
        WARN("no video decoder, skipping");
        return;
    }
    m.begin_stepping(0);
    double last_start = -1;
    quint64 serial = m.serial();
    int frames = 0;
    for (double t = 0; t < 1000; t += 1000.0 / 30, ++frames) {
        REQUIRE(wait_until([&] { return m.shows(t); }, 3000));
        const auto [from, to] = m.frame_span();
        CHECK(from <= t + 1);
        CHECK(to > t);
        CHECK(from >= last_start);
        last_start = from;
        const quint64 now = m.serial();
        CHECK(now >= serial);
        serial = now;
        CHECK(m.shows(t)); // the same t again: nothing new
        CHECK(m.serial() == serial);
    }
    CHECK(m.stepping_gaps() == 0);
    CHECK(wait_until([&] { return m.shows(double(m.duration_ms()) + 500); }, 5000)); // past the end: held
    m.end_stepping();
}

TEST_CASE("MediaSource stepping starts at the seek target even while the video is playing (section 26)") {
    sv::media::MediaSource m;
    const auto wait_until = [](auto pred, int ms) {
        QElapsedTimer t;
        t.start();
        while (!pred() && t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return pred();
    };
    m.open("video", SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4");
    if (!wait_until([&] { return m.duration_ms() > 0; }, 5000)) {
        WARN("no video decoder, skipping");
        return;
    }
    for (const double target : {1500.0, 300.0, 1200.0}) { // ahead of, behind and again ahead of the live position
        m.set_playback(true, true, 1.0);
        wait_until([&] { return false; }, 400); // frames keep arriving from the live position
        m.begin_stepping(qint64(target) + 1);
        REQUIRE(wait_until([&] { return m.shows(target); }, 3000));
        const auto [from, to] = m.frame_span();
        CHECK(from <= target + 1);
        CHECK(from > target - 200);
        CHECK(to > target);
        m.end_stepping();
    }
}

TEST_CASE("Offline render needs no window, and refuses without a path and while recording") {
    Session s(nullptr);
    s.set_project(sv::demo_project());
    const QString mp4 = QString::fromStdString((std::filesystem::temp_directory_path() / "sv_render.mp4").string());
    CHECK(s.startRender({}) == "Give the mp4 to render to");
    // No window at all: the render draws on its own device, as it must while the app is minimised. Cinematic (the
    // default) with motion-blur sub-frames.
    const QString e =
        s.startRender({{"path", mp4}, {"duration", 300}, {"fps", 10}, {"width", 64}, {"height", 36}, {"samples", 2}});
    if (e.isEmpty()) {
        QElapsedTimer clock;
        clock.start();
        while (s.recording()["render"].toMap()["active"].toBool() && clock.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QVariantMap r = s.recording()["render"].toMap();
        CHECK(r["error"].toString() == "");
        CHECK(r["frame"].toInt() == 3);
        // Progress for the render screen and MCP get_render: stage times, rate, device, preview.
        const QVariantMap st = r["stages"].toMap();
        for (const char *k : {"waitVideo", "waitDmx", "waitEncoder", "build", "draw", "readback", "push"}) {
            REQUIRE(st.contains(k));
            CHECK(st[k].toDouble() >= 0);
        }
        CHECK(st["draw"].toDouble() > 0);
        CHECK(r["elapsedMs"].toLongLong() > 0);
        CHECK(r["previewSerial"].toULongLong() >= 1);
        CHECK_FALSE(r["backend"].toString().isEmpty());
        CHECK(r["quality"].toString() == "cinematic");
        CHECK(r["supersample"].toBool());
        CHECK(r["uiMs"].toDouble() >= 0);
        CHECK(s.renderPreview().width() == 64);
        // Without supersampling the draw is the output size; the job still completes.
        REQUIRE(
            s.startRender(
                 {{"path", mp4}, {"duration", 300}, {"fps", 10}, {"width", 64}, {"height", 36}, {"supersample", false}})
                .isEmpty());
        clock.restart();
        while (s.recording()["render"].toMap()["active"].toBool() && clock.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QVariantMap r2 = s.recording()["render"].toMap();
        CHECK(r2["frame"].toInt() == 3);
        CHECK_FALSE(r2["supersample"].toBool());
    } else {
        CHECK(e == "No graphics device to render with"); // offscreen platform without GL
    }
    CHECK_FALSE(s.recording()["render"].toMap()["active"].toBool());
    if (e.isEmpty()) { // stopping while a frame is on the worker thread waits for it and closes the file
        REQUIRE(s.startRender({{"path", mp4},
                               {"duration", 10000},
                               {"fps", 10},
                               {"width", 64},
                               {"height", 36},
                               {"samples", 1},
                               {"quality", "high"}})
                    .isEmpty());
        QElapsedTimer clock;
        clock.start();
        while (s.recording()["render"].toMap()["frame"].toInt() < 2 && clock.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        s.stopRender();
        CHECK_FALSE(s.recording()["render"].toMap()["active"].toBool());
        CHECK(s.recording()["render"].toMap()["error"].toString() == "Stopped");
    }
    // A composition video steps frame by frame, without gaps.
    if (e.isEmpty() &&
        s.setMediaFile("video", QUrl::fromLocalFile(SV_SOURCE_DIR "/tests/fixtures/media/testsrc.mp4")).isEmpty()) {
        QElapsedTimer clock;
        clock.start();
        while (s.media_status()["width"].toInt() == 0 && clock.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        REQUIRE(s.startRender({{"path", mp4},
                               {"duration", 1000},
                               {"fps", 10},
                               {"width", 64},
                               {"height", 36},
                               {"samples", 1},
                               {"quality", "high"}})
                    .isEmpty());
        clock.restart();
        while (s.recording()["render"].toMap()["active"].toBool() && clock.elapsed() < 60000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QVariantMap r = s.recording()["render"].toMap();
        CHECK(r["error"].toString() == "");
        CHECK(r["frame"].toInt() == 10);
        CHECK(r["videoGaps"].toInt() == 0);
    }
    const QString base = QString::fromStdString((std::filesystem::temp_directory_path() / "sv_render_take").string());
    REQUIRE(s.startRecording(QUrl::fromLocalFile(base), false).isEmpty());
    CHECK(s.startRender({{"path", mp4}}) == "Stop recording before rendering");
    s.stopRecording();
}
