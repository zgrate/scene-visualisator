#include "core/project_file.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using sv::json;

namespace {

// A fresh, empty directory per test.
struct TempDir {
    fs::path dir = fs::temp_directory_path() / ("sv_test_" + sv::new_uuid());
    TempDir() { fs::create_directories(dir); }
    ~TempDir() { fs::remove_all(dir); }
};

sv::Package sample() {
    sv::Package pkg;
    sv::json fixture = sv::new_fixture("Spot 1", "Generic@Spot@1.gdtf", "Standard");
    fixture["patch"] = {{"universe", 1}, {"address", 1}};
    fixture["futureField"] = {{"keep", true}}; // a key this version does not know
    pkg.project["objects"].push_back(fixture);
    pkg.project["objects"].push_back(sv::new_led_wall("Main"));
    pkg.project["futureTopLevel"] = 42;
    pkg.files["gdtf/Generic@Spot@1.gdtf"] = {0x50, 0x4B, 0x03, 0x04, 0x00, 0xFF};
    return pkg;
}

void write_text(const fs::path &p, const std::string &s) { std::ofstream(p, std::ios::binary) << s; }

} // namespace

TEST_CASE("FR-1 package round trip keeps document, unknown keys and assets") {
    TempDir t;
    const fs::path path = t.dir / "Show.svproj";
    const sv::Package pkg = sample();
    sv::save_package(path, pkg);
    const sv::Package back = sv::load_package(path);
    CHECK(back.project == pkg.project);
    CHECK(back.project["futureTopLevel"] == 42);
    CHECK(back.project["objects"][0]["futureField"]["keep"] == true);
    CHECK(back.files == pkg.files);

    // load -> save -> load gives the same project.json text
    const fs::path again = t.dir / "Again.svproj";
    sv::save_package(again, back);
    CHECK(sv::load_package(again).project.dump(2) == pkg.project.dump(2));
}

TEST_CASE("saves keep three rolling backups") {
    TempDir t;
    const fs::path path = t.dir / "Show.svproj";
    sv::Package pkg;
    for (int i = 1; i <= 5; ++i) {
        pkg.project["venue"]["width"] = i;
        sv::save_package(path, pkg);
    }
    CHECK(sv::load_package(path).project["venue"]["width"] == 5);
    CHECK(sv::load_package(path.string() + ".bak1").project["venue"]["width"] == 4);
    CHECK(sv::load_package(path.string() + ".bak3").project["venue"]["width"] == 2);
    CHECK_FALSE(fs::exists(path.string() + ".bak4"));
}

TEST_CASE("AT-6 a write killed half way leaves the previous package valid") {
    TempDir t;
    const fs::path path = t.dir / "Show.svproj";
    const sv::Package pkg = sample();
    sv::save_package(path, pkg);
    write_text(path.string() + ".tmp", "PK\x03\x04 truncated"); // what a crash mid-write leaves behind
    CHECK(sv::load_package(path).project == pkg.project);
    sv::save_package(path, pkg); // the stale temp file does not block the next save
    CHECK_FALSE(fs::exists(path.string() + ".tmp"));
}

TEST_CASE("corrupt, foreign and newer-schema files are refused") {
    TempDir t;
    const fs::path junk = t.dir / "junk.svproj";
    write_text(junk, "not a zip");
    CHECK_THROWS_AS(sv::load_package(junk), sv::ProjectFileError);
    CHECK_THROWS_AS(sv::load_package(t.dir / "missing.svproj"), sv::ProjectFileError);

    sv::Package newer;
    newer.project["schemaVersion"] = sv::schema_version + 1;
    sv::save_package(t.dir / "newer.svproj", newer);
    CHECK_THROWS_AS(sv::load_package(t.dir / "newer.svproj"), sv::ProjectFileError);

    sv::Package no_version;
    no_version.project.erase("schemaVersion");
    sv::save_package(t.dir / "nov.svproj", no_version);
    CHECK_THROWS_AS(sv::load_package(t.dir / "nov.svproj"), sv::ProjectFileError);
}

TEST_CASE("AT-6 autosave is offered until the next real save") {
    TempDir t;
    const fs::path path = t.dir / "Show.svproj";
    sv::save_package(path, sample());
    CHECK_FALSE(sv::has_newer_autosave(path));
    sv::Package edited = sample();
    edited.project["venue"]["width"] = 33;
    sv::write_autosave(path, edited);
    CHECK(sv::has_newer_autosave(path));
    CHECK(sv::load_package(sv::autosave_path(path)).project["venue"]["width"] == 33);
    sv::save_package(path, edited);
    CHECK_FALSE(sv::has_newer_autosave(path));
}

TEST_CASE("an alpha (schema 1) package migrates to the current schema") {
    TempDir t;
    sv::Package alpha = sample();
    alpha.project["schemaVersion"] = 1;
    alpha.project["composition"]["source"] = "testPattern";
    alpha.project.erase("layers");
    alpha.project.erase("outputs");
    sv::save_package(t.dir / "alpha.svproj", alpha);

    const json p = sv::load_package(t.dir / "alpha.svproj").project;
    CHECK(p["schemaVersion"] == sv::schema_version);
    CHECK(p["composition"]["source"] == json{{"kind", "testPattern"}});
    CHECK(p["layers"].is_array());
    CHECK(p["outputs"].is_array());
    CHECK(p["futureTopLevel"] == 42);
    CHECK(p["objects"] == alpha.project["objects"]);
}
