#include "session.h"

#include "core/interchange.h"
#include "core/project_file.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTimer>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <ranges>

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
    CHECK(t.file_path() == QString::fromStdString(path.string()));
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

int main(int argc, char *argv[]) {
    QStandardPaths::setTestModeEnabled(true);           // the untitled autosave goes to a test location
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) // also covers test discovery at build time
        qputenv("QT_QPA_PLATFORM", "offscreen");
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
