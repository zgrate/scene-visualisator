#include "core/mvr.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <random>

using namespace sv;
using Catch::Approx;

namespace {

std::vector<std::uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// A hand-made MVR in the spec's shape: two layers, a group with a child fixture, a screen and a scene object with
// glTF models, a truss with a 3DS model, AUXData, and children and attributes we do not interpret.
std::vector<std::uint8_t> sample_mvr() {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<GeneralSceneDescription verMajor="1" verMinor="6" provider="test" providerVersion="1">
  <Scene>
    <AUXData><Class name="Lights" uuid="C1"/></AUXData>
    <Layers>
      <Layer name="Rig" uuid="L1">
        <ChildList>
          <GroupObject name="Pipe" uuid="G1">
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,6000}</Matrix>
            <ChildList>
              <Fixture name="Spot 1" uuid="F1" multipatch="">
                <Matrix>{1,0,0}{0,1,0}{0,0,1}{1000,2000,0}</Matrix>
                <Classing>C1</Classing>
                <GDTFSpec>Robe@Spot.gdtf</GDTFSpec>
                <GDTFMode>Mode 1</GDTFMode>
                <Focus>P1</Focus>
                <FixtureID>101</FixtureID>
                <UnitNumber>0</UnitNumber>
                <Addresses><Address break="0">2.1</Address></Addresses>
                <CustomId>7</CustomId>
              </Fixture>
              <Fixture name="Spot 2" uuid="F2">
                <Matrix>{0,1,0}{-1,0,0}{0,0,1}{0,0,0}</Matrix>
                <GDTFSpec>Robe@Spot</GDTFSpec>
                <GDTFMode>Mode 1</GDTFMode>
                <FixtureID>A2</FixtureID>
                <Addresses><Address break="0">1025</Address><Address break="1">20</Address></Addresses>
              </Fixture>
            </ChildList>
          </GroupObject>
        </ChildList>
      </Layer>
      <Layer name="Set" uuid="L2">
        <ChildList>
          <VideoScreen name="Screen" uuid="V1">
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,-3000,0}</Matrix>
            <Geometries><Geometry3D fileName="screen.glb"/></Geometries>
            <Sources><Source linkedGeometry="screen" type="NDI">Arena</Source></Sources>
          </VideoScreen>
          <SceneObject name="Riser" uuid="S1">
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,0}</Matrix>
            <Geometries><Geometry3D fileName="box.glb"/></Geometries>
          </SceneObject>
          <Truss name="Truss" uuid="T1">
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,7000}</Matrix>
            <Geometries><Geometry3D fileName="truss.3ds"/></Geometries>
          </Truss>
        </ChildList>
      </Layer>
    </Layers>
  </Scene>
</GeneralSceneDescription>
)";
    std::vector<float> box;
    for (float x : {-1.f, 1.f})
        box.insert(box.end(), {x, 0, 0, x, 1, 0, x, 1, 1}); // two triangles spanning 2 x 1 x 1
    return write_zip({{"GeneralSceneDescription.xml", bytes(xml)},
                      {"Robe@Spot.gdtf", bytes("gdtf bytes")},
                      {"screen.glb", write_glb({-2, 0, 0, 2, 0, 0, 2, 2.5f, 0})},
                      {"box.glb", write_glb(box)},
                      {"truss.3ds", bytes("3ds bytes")}});
}

const json *find(const json &objects, const std::string &uuid) {
    for (const json &o : objects)
        if (o.value("uuid", std::string()) == uuid)
            return &o;
    return nullptr;
}

} // namespace

TEST_CASE("MVR matrices convert to our transform and back (FR-22)") {
    // MVR mm, Z up, Y upstage -> metres, Y up, Z toward the audience.
    Transform t = transform_from_mvr({1, 0, 0, 0, 1, 0, 0, 0, 1, 1000, 2000, 3000});
    CHECK(t.position == std::array<double, 3>{1, 3, -2});
    CHECK(t.rotation == std::array<double, 3>{0, 0, 0});
    // A quarter turn about MVR Z (x axis onto upstage) is a quarter turn about our Y.
    t = transform_from_mvr({0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0});
    CHECK(t.rotation[1] == Approx(90));
    CHECK(t.rotation[0] == Approx(0).margin(1e-9));

    std::mt19937 rng(7);
    std::uniform_real_distribution<double> angle(-180, 180), pitch(-89, 89), scale(0.5, 2), pos(-20000, 20000);
    for (int i = 0; i < 50; ++i) {
        const Transform in{{pos(rng) / 1000, pos(rng) / 1000, pos(rng) / 1000},
                           {pitch(rng), angle(rng), angle(rng)},
                           {scale(rng), scale(rng), scale(rng)}};
        const std::array<double, 12> m = mvr_from_transform(in);
        const std::array<double, 12> back = mvr_from_transform(transform_from_mvr(m));
        for (std::size_t k = 0; k < 12; ++k)
            CHECK(back[k] == Approx(m[k]).margin(k < 9 ? 1e-6 : 1e-3));
    }
}

TEST_CASE("MVR import maps layers, hierarchy, patch, GDTF and geometry (FR-22)") {
    const MvrScene s = mvr_import(sample_mvr());
    REQUIRE(s.layers.size() == 2);
    CHECK(s.layers[0]["name"] == "Rig");
    REQUIRE(s.objects.size() == 6);
    CHECK(s.aux_data.find("<Class name=\"Lights\"") != std::string::npos);

    const json &group = *find(s.objects, "G1");
    CHECK(group["type"] == "group");
    CHECK(group["position"] == json({0.0, 6.0, 0.0}));

    const json &f1 = *find(s.objects, "F1");
    CHECK(f1["type"] == "fixture");
    CHECK(f1["parent"] == "G1");
    CHECK(f1["layer"] == "L1");
    CHECK(f1["gdtf"] == "Robe@Spot.gdtf");
    CHECK(f1["mode"] == "Mode 1");
    CHECK(f1["fixtureId"] == 101);
    CHECK(f1["patch"] == json({{"universe", 2}, {"address", 1}}));
    CHECK(f1["position"] == json({1.0, 0.0, -2.0}));
    const std::string kept = f1["extras"]["mvr"]["xml"];
    CHECK(kept.find("<Focus>P1</Focus>") != std::string::npos);
    CHECK(kept.find("<CustomId>7</CustomId>") != std::string::npos);
    CHECK(kept.find("GDTFSpec") == std::string::npos);
    CHECK(f1["extras"]["mvr"]["attributes"]["multipatch"] == "");

    const json &f2 = *find(s.objects, "F2");
    CHECK(f2["gdtf"] == "Robe@Spot.gdtf"); // the extension is found
    CHECK(f2["fixtureId"].is_null());      // "A2" is not numeric, so it stays verbatim
    CHECK(f2["patch"] == json({{"universe", 3}, {"address", 1}}));
    CHECK(f2["rotation"][1] == 90.0);
    CHECK(f2["extras"]["mvr"]["xml"].get<std::string>().find("break=\"1\"") != std::string::npos);

    const json &screen = *find(s.objects, "V1");
    CHECK(screen["type"] == "ledWall");
    CHECK(screen["width"] == 4.0);
    CHECK(screen["height"] == 2.5);
    CHECK(screen["position"] == json({0.0, 0.0, 3.0}));
    CHECK(find(s.objects, "S1")->at("geometry") == "geometry/box.glb");
    CHECK(find(s.objects, "T1")->at("type") == "truss");

    CHECK(s.files.contains("gdtf/Robe@Spot.gdtf"));
    CHECK(s.files.contains("geometry/truss.3ds"));
    CHECK(s.files.contains("geometry/box.glb"));
    CHECK(s.warnings.empty());
}

TEST_CASE("MVR import, export and import again loses nothing (AT-1)") {
    const MvrScene a = mvr_import(sample_mvr());
    json project = new_project();
    project["objects"] = a.objects;
    project["layers"] = a.layers;
    project["mvr"] = {{"auxData", a.aux_data}};
    ZipEntries files = a.files;
    const MvrScene b = mvr_import(mvr_export(project, files));
    CHECK(b.objects == a.objects);
    CHECK(b.layers == a.layers);
    CHECK(b.aux_data == a.aux_data);
    CHECK(b.files == a.files);
}

TEST_CASE("MVR export of our own rig generates geometry and re-imports (FR-22)") {
    json project = demo_project();
    json strip = new_pixel_strip("Strip");
    project["objects"].push_back(strip);
    ZipEntries files;
    for (const json &o : project["objects"])
        if (o["type"] == "fixture")
            files["gdtf/" + o["gdtf"].get<std::string>()] = bytes("gdtf");
    const MvrScene s = mvr_import(mvr_export(project, files));
    CHECK(s.warnings.empty());
    CHECK(s.layers.size() == 1);
    std::size_t exported = 0;
    for (const json &o : project["objects"]) {
        if (o["type"] == "pixelStrip" || o["type"] == "camera" || o["type"] == "deck" || o["type"] == "shape")
            continue; // no MVR counterpart
        ++exported;
        const json *r = find(s.objects, o["uuid"]);
        REQUIRE(r);
        CHECK(r->at("type") == o["type"]);
        CHECK(r->at("name") == o["name"]);
        for (int i = 0; i < 3; ++i) {
            CHECK(r->at("position")[i].get<double>() == Approx(o["position"][i].get<double>()).margin(1e-6));
            CHECK(r->at("rotation")[i].get<double>() == Approx(o["rotation"][i].get<double>()).margin(1e-5));
        }
        if (o["type"] == "fixture") {
            CHECK(r->at("patch") == o["patch"]);
            CHECK(r->at("mode") == o["mode"]);
            CHECK(r->at("fixtureId") == o["fixtureId"]);
        } else if (o["type"] == "ledWall") {
            CHECK(r->at("width").get<double>() == Approx(o["width"].get<double>()));
            CHECK(r->at("height").get<double>() == Approx(o["height"].get<double>()));
        } else if (o["type"] == "truss") {
            CHECK(r->at("length").get<double>() == Approx(o["length"].get<double>()));
        }
    }
    CHECK(s.objects.size() == exported);
}

TEST_CASE("glTF writer output reads back with flat normals") {
    const std::vector<ObjGroup> g = parse_gltf(write_glb({0, 0, 0, 1, 0, 0, 0, 1, 0}));
    REQUIRE(g.size() == 1);
    REQUIRE(g[0].vertices.size() == 18);
    CHECK(g[0].vertices[6] == 1.0f);
    CHECK(g[0].vertices[5] == 1.0f); // normal +Z
    CHECK_THROWS_AS(parse_gltf(bytes("nope")), InterchangeError);
    CHECK_THROWS_AS(mvr_import(bytes("nope")), InterchangeError);
}

TEST_CASE("grandMA set pieces import as trusses and groups, and export as they came (FR-22)") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<GeneralSceneDescription verMajor="1" verMinor="6">
  <Scene>
    <Layers>
      <Layer name="Set" uuid="L1">
        <ChildList>
          <Fixture name="Universal" uuid="U1">
            <GDTFSpec>Generic@Universal.gdtf</GDTFSpec><GDTFMode>Default</GDTFMode>
          </Fixture>
          <Fixture name="stage" uuid="G1">
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,0}</Matrix>
            <ChildList>
              <Fixture name="upright" uuid="T1">
                <Matrix>{1,0,0}{0,0,1}{0,-1,0}{1000,8300,1250}</Matrix>
                <GDTFSpec>Set@eurotruss_FD34-250.gdtf</GDTFSpec><GDTFMode>Default</GDTFMode>
                <FixtureID>5</FixtureID>
              </Fixture>
              <Fixture name="dimmer" uuid="D1">
                <Matrix>{1,0,0}{0,1,0}{0,0,1}{0,0,0}</Matrix>
                <GDTFSpec>Generic@Dimmer.gdtf</GDTFSpec><GDTFMode>Mode 0</GDTFMode>
              </Fixture>
              <Fixture name="corner" uuid="T2">
                <Matrix>{1,0,0}{0,1,0}{0,0,1}{4650,8300,2950}</Matrix>
                <GDTFSpec>Set@eurotruss_FD34-COR.gdtf</GDTFSpec><GDTFMode>Default</GDTFMode>
              </Fixture>
            </ChildList>
            <GDTFSpec>Generic@Grouping.gdtf</GDTFSpec><GDTFMode>Default</GDTFMode>
          </Fixture>
        </ChildList>
      </Layer>
    </Layers>
  </Scene>
</GeneralSceneDescription>
)";
    const auto resolve = [](const std::string &spec, const std::string &) {
        return spec == "Generic@Dimmer.gdtf" ? std::pair<std::string, std::string>("Generic@Dimmer.gdtf", "1ch")
                                             : std::pair<std::string, std::string>();
    };
    const MvrScene a = mvr_import(write_zip({{"GeneralSceneDescription.xml", bytes(xml)}}), resolve);
    CHECK(a.warnings.empty());
    REQUIRE(a.objects.size() == 5);
    CHECK(find(a.objects, "D1")->at("mode") == "1ch");
    CHECK(find(a.objects, "U1")->at("type") == "group");
    CHECK(find(a.objects, "G1")->at("type") == "group");
    const json &up = *find(a.objects, "T1");
    CHECK(up["type"] == "truss");
    CHECK(up["parent"] == "G1");
    CHECK(up["length"].get<double>() == Approx(2.5));
    CHECK(up["position"][2].get<double>() == Approx(-8.3));
    CHECK(up["rotation"] == json::array({0.0, 90.0, 90.0})); // Ry·Rz turns our X (the bar) up to Y
    const json &corner = *find(a.objects, "T2");
    CHECK(corner["trussType"] == "corner");
    CHECK(corner["length"].get<double>() == Approx(0.29));

    json project = new_project();
    project["objects"] = a.objects;
    project["layers"] = a.layers;
    ZipEntries files;
    const auto zip = mvr_export(project, files);
    const auto entries = read_zip(zip);
    const std::string out(entries.at("GeneralSceneDescription.xml").begin(),
                          entries.at("GeneralSceneDescription.xml").end());
    CHECK(out.find("<Matrix>{1,0,0}{0,0,1}{0,-1,0}{1000,8300,1250}</Matrix>") != std::string::npos);
    CHECK(out.find("<GDTFSpec>Set@eurotruss_FD34-250.gdtf</GDTFSpec>") != std::string::npos);
    CHECK(out.find("<Fixture name=\"stage\"") != std::string::npos);
    CHECK(mvr_import(zip, resolve).objects == a.objects);
}
