#include "gdtf/dmx_value.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("coarse/fine DMX slots combine big-endian") {
    const std::array<std::uint8_t, 4> frame{0x80, 0x00, 0xFF, 0xFF};
    CHECK(sv::gdtf::dmx_value(frame, 0, 1) == 0x80);
    CHECK(sv::gdtf::dmx_value(frame, 0, 2) == 0x8000);
    CHECK(sv::gdtf::dmx_normalised(sv::gdtf::dmx_value(frame, 2, 2), 2) == 1.0);
    CHECK(sv::gdtf::dmx_value(frame, 3, 2) == 0xFF00); // past the frame end reads as 0
}

#include "gdtf/decoder.h"

#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

using namespace sv::gdtf;
using Catch::Approx;

namespace {

FixtureType generic(const char *file) {
    return load_gdtf(std::filesystem::path(SV_SOURCE_DIR) / "resources/gdtf" / file);
}

std::array<std::uint8_t, 512> frame_at(std::uint16_t address, std::initializer_list<std::uint8_t> values) {
    std::array<std::uint8_t, 512> f{};
    std::ranges::copy(values, f.begin() + (address - 1));
    return f;
}

} // namespace

TEST_CASE("FR-10: generic spot decodes like the POC self-test (dimmer, red, centre pan/tilt)") {
    const auto spot = generic("Generic@Spot_7ch.gdtf");
    CHECK(spot.name == "Spot 7ch");
    REQUIRE(spot.modes.size() == 1);
    const Mode &m = spot.modes[0];
    CHECK(m.footprint == 7);
    const auto a = decode(spot, m, frame_at(1, {255, 128, 128, 255, 0, 0}), 1);
    CHECK(a.dimmer == 1.0);
    CHECK(a.colour == Rgb{1, 0, 0});
    CHECK(std::abs(a.pan) < 2);
    CHECK(std::abs(a.tilt) < 2);
    CHECK(intensity_offsets(m) == std::vector<std::uint16_t>{0});
    // Zoom 0..255 -> 8..40 degrees; full pan is +270.
    const auto b = decode(spot, m, frame_at(100, {0, 255, 0, 0, 0, 0, 255}), 100);
    CHECK(b.pan == Approx(270));
    CHECK(b.tilt == Approx(-135));
    CHECK(b.beam_angle == Approx(40));
}

TEST_CASE("Colour wheel slots follow ChannelSets and filter the emitter") {
    const auto beam = generic("Generic@Beam_5ch.gdtf");
    const Mode &m = beam.modes[0];
    REQUIRE(beam.wheel("Colors"));
    CHECK(beam.wheel("Colors")->slots.size() == 8);
    auto a = decode(beam, m, frame_at(1, {255, 0, 0, 0, 0}), 1);
    CHECK(a.colour_slot == 1);
    CHECK(a.colour[1] == Approx(1).margin(0.02)); // open = white
    a = decode(beam, m, frame_at(1, {255, 0, 0, 40, 70}), 1);
    CHECK(a.colour_slot == 2); // 32..63 = red
    CHECK(a.colour[0] > 0.9f);
    CHECK(a.colour[1] < 0.2f);
    CHECK(a.gobo_slot == 3);
    CHECK(a.beam_angle == Approx(4));
}

TEST_CASE("Wash RGBW adds white; blinder uses its warm emitter") {
    const auto wash = generic("Generic@Wash_RGBW.gdtf");
    auto a = decode(wash, wash.modes[0], frame_at(1, {255, 0, 0, 0, 255}), 1);
    CHECK(a.colour == Rgb{1, 1, 1});
    a = decode(wash, wash.modes[0], frame_at(1, {0, 0, 0, 0, 0}), 1);
    CHECK(a.dimmer == 0);
    const auto blinder = generic("Generic@Blinder.gdtf");
    a = decode(blinder, blinder.modes[0], frame_at(1, {255}), 1);
    CHECK(a.colour[0] == Approx(1));
    CHECK(a.colour[2] < a.colour[1]);
    CHECK(a.colour[1] < a.colour[0]);
}

TEST_CASE("Pixel bar decodes one colour per cell geometry") {
    const auto bar = generic("Generic@Pixel_Bar_8.gdtf");
    CHECK(bar.modes[0].footprint == 25);
    auto f = frame_at(1, {255, 255, 0, 0});
    f[22] = 255, f[23] = 255, f[24] = 255; // cell 8 white
    const auto a = decode(bar, bar.modes[0], f, 1);
    REQUIRE(a.cells.size() == 8);
    CHECK(a.cells[0] == Rgb{1, 0, 0});
    CHECK(a.cells[1] == Rgb{0, 0, 0});
    CHECK(a.cells[7] == Rgb{1, 1, 1});
}

TEST_CASE("16-bit channels combine coarse/fine and footprints past 512 read as zero") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><DMXModes><DMXMode Name="16bit"><DMXChannels>
        <DMXChannel Offset="1,2"><LogicalChannel Attribute="Pan">
          <ChannelFunction Attribute="Pan" DMXFrom="0/1" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="None"><LogicalChannel Attribute="Dimmer"><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3"><LogicalChannel Attribute="Shutter1">
          <ChannelFunction Name="Closed" Attribute="Shutter1" DMXFrom="0/1"/>
          <ChannelFunction Name="Open" Attribute="Shutter1" DMXFrom="32/1"/>
          <ChannelFunction Name="Strobe" Attribute="Shutter1Strobe" DMXFrom="64/1" PhysicalFrom="1" PhysicalTo="20"/>
        </LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("16bit");
    CHECK(m.channels.size() == 2); // virtual channel skipped
    CHECK(m.footprint == 3);
    auto a = decode(t, m, frame_at(1, {0x80, 0x00, 0}), 1);
    CHECK(std::abs(a.pan) < 0.01);
    CHECK_FALSE(a.shutter_open);
    CHECK(a.dimmer == 1); // no dimmer channel = always on
    a = decode(t, m, frame_at(1, {0, 0, 255}), 1);
    CHECK(a.strobe_hz == Approx(20));
    a = decode(t, m, frame_at(512, {0xFF}), 512); // coarse at slot 512, fine and shutter past the end read 0
    CHECK(a.pan == Approx(-270 + 540.0 * 0xFF00 / 0xFFFF));
    CHECK_FALSE(a.shutter_open);
}

TEST_CASE("encode is decode in reverse: 16-bit pan, open shutter, wheel slot and one head of two") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><Wheels>
        <Wheel Name="G1"><Slot Name="Open"/><Slot Name="A"/><Slot Name="B"/></Wheel></Wheels>
        <DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1,2"><LogicalChannel Attribute="Pan">
          <ChannelFunction Attribute="Pan" DMXFrom="0/1" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3"><LogicalChannel Attribute="Shutter1">
          <ChannelFunction Name="Closed" Attribute="Shutter1" DMXFrom="0/1"/>
          <ChannelFunction Name="Open" Attribute="Shutter1" DMXFrom="32/1"/>
          <ChannelFunction Name="Strobe" Attribute="Shutter1Strobe" DMXFrom="64/1" PhysicalFrom="1" PhysicalTo="20"/>
        </LogicalChannel></DMXChannel>
        <DMXChannel Offset="4"><LogicalChannel Attribute="Gobo1"><ChannelFunction Attribute="Gobo1" Wheel="G1">
          <ChannelSet DMXFrom="0/1" WheelSlotIndex="1"/><ChannelSet DMXFrom="8/1" WheelSlotIndex="2"/>
          <ChannelSet DMXFrom="16/1" WheelSlotIndex="3"/></ChannelFunction></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("m");
    std::vector<std::uint8_t> f(512, 0);
    const auto apply = [&](const std::vector<DmxByte> &bytes) {
        for (const DmxByte &b : bytes)
            f[b.offset] = b.value;
    };
    apply(encode(m, "Pan", 90));
    apply(encode(m, "Gobo1", 3));
    apply(encode(m, "Shutter1Strobe", 10));
    auto a = decode(t, m, f, 1);
    CHECK(a.pan == Approx(90).margin(0.01));
    CHECK(a.gobo_slot == 3);
    CHECK(a.strobe_hz == Approx(10).margin(0.1));
    apply(open_shutter(m));
    a = decode(t, m, f, 1);
    CHECK(a.shutter_open);
    CHECK(a.strobe_hz == 0);
    CHECK(encode(m, "Tilt", 10).empty());

    // Two heads: head 1 alone, or every head with -1.
    const auto two = parse_description(R"(<GDTF><FixtureType Name="X"><Geometries><Geometry Name="Base">
        <Axis Name="Yoke 1"><Axis Name="Head 1"/></Axis><Axis Name="Yoke 2"><Axis Name="Head 2"/></Axis>
        </Geometry></Geometries><DMXModes><DMXMode Name="m" Geometry="Base"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Yoke 1"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Yoke 2"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m2 = *two.mode("m");
    REQUIRE(m2.heads.size() == 2);
    const auto one = encode(m2, "Pan", 270, 1);
    REQUIRE(one.size() == 1);
    CHECK(one[0].offset == 1);
    CHECK(one[0].value == 255);
    CHECK(encode(m2, "Pan", -270).size() == 2);
}

TEST_CASE("CMY flags subtract their complement and CTO warms toward its Kelvin value") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1,2"><LogicalChannel Attribute="ColorSub_C"><ChannelFunction Attribute="ColorSub_C"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3"><LogicalChannel Attribute="ColorSub_Y"><ChannelFunction Attribute="ColorSub_Y"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="4"><LogicalChannel Attribute="CTO">
          <ChannelFunction Attribute="CTO" PhysicalFrom="7500" PhysicalTo="3200"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("m");
    auto a = decode(t, m, frame_at(1, {0, 0, 0, 0}), 1);
    CHECK(a.colour[0] == Approx(1));
    CHECK(a.colour[2] == Approx(1));
    a = decode(t, m, frame_at(1, {0xFF, 0xFF, 0x80, 0}), 1); // full cyan, half yellow: green with less blue
    CHECK(a.colour[0] == Approx(0).margin(1e-6));
    CHECK(a.colour[1] == Approx(1));
    CHECK(a.colour[2] == Approx(0.5).margin(0.01));
    a = decode(t, m, frame_at(1, {0, 0, 0, 0xFF}), 1); // full CTO: red stays, blue drops
    CHECK(a.colour[0] == Approx(1));
    CHECK(a.colour[2] < 0.6f);
    CHECK(a.colour[1] > a.colour[2]);
}

TEST_CASE("A second gobo wheel left open keeps the first wheel's gobo") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><Wheels>
        <Wheel Name="G1"><Slot Name="Open"/><Slot Name="A"/><Slot Name="B"/></Wheel>
        <Wheel Name="G2"><Slot Name="Open"/><Slot Name="C"/></Wheel></Wheels>
        <DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1"><LogicalChannel Attribute="Gobo1"><ChannelFunction Attribute="Gobo1" Wheel="G1">
          <ChannelSet DMXFrom="0/1" WheelSlotIndex="1"/><ChannelSet DMXFrom="8/1" WheelSlotIndex="2"/></ChannelFunction></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2"><LogicalChannel Attribute="Gobo2"><ChannelFunction Attribute="Gobo2" Wheel="G2">
          <ChannelSet DMXFrom="0/1" WheelSlotIndex="1"/><ChannelSet DMXFrom="4/1" WheelSlotIndex="2"/></ChannelFunction></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("m");
    CHECK(decode(t, m, frame_at(1, {8, 0}), 1).gobo_slot == 2);
    CHECK(decode(t, m, frame_at(1, {0, 4}), 1).gobo_slot == 2);
    CHECK(decode(t, m, frame_at(1, {0, 0}), 1).gobo_slot == 1);
}

TEST_CASE("Channels on a referenced geometry repeat once per GeometryReference at its Break offset") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="Bar"><Geometries>
        <Geometry Name="Cell"/>
        <Geometry Name="Body"><Geometry Name="Cells">
          <GeometryReference Name="Cell 1" Geometry="Cell"><Break DMXBreak="1" DMXOffset="2"/></GeometryReference>
          <GeometryReference Name="Cell 2" Geometry="Cell"><Break DMXBreak="1" DMXOffset="5"/></GeometryReference>
        </Geometry></Geometry>
      </Geometries><DMXModes><DMXMode Name="7ch" Geometry="Body"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Cells"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="1" Geometry="Cell"><LogicalChannel><ChannelFunction Attribute="ColorAdd_R"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Cell"><LogicalChannel><ChannelFunction Attribute="ColorAdd_G"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="Cell"><LogicalChannel><ChannelFunction Attribute="ColorAdd_B"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("7ch");
    CHECK(m.channels.size() == 7);
    CHECK(m.footprint == 7);
    const auto a = decode(t, m, frame_at(1, {255, 255, 0, 0, 0, 0, 255}), 1);
    REQUIRE(a.cells.size() == 2);
    CHECK(a.cells[0] == Rgb{1, 0, 0});
    CHECK(a.cells[1] == Rgb{0, 0, 1});
}

TEST_CASE("FR-10 warm and cool white cells light and the first Dimmer is the master") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1"><LogicalChannel Attribute="Dimmer"><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="A"><LogicalChannel><ChannelFunction Attribute="ColorAdd_CW"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="B"><LogicalChannel><ChannelFunction Attribute="ColorAdd_WW"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="4"><LogicalChannel Attribute="Dimmer"><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const auto a = decode(t, *t.mode("m"), frame_at(1, {255, 255, 255, 0}), 1);
    CHECK(a.dimmer == Approx(1)); // a sub-dimmer at 0 does not black out the master
    REQUIRE(a.cells.size() == 2);
    CHECK(a.cells[0][2] > 0.9f); // cool white: blue near full
    CHECK(a.cells[1][0] == Approx(1));
    CHECK(a.cells[1][2] < 0.6f); // warm white: less blue
}

TEST_CASE("FR-10 a multi-cell fixture's colour is the mean of its cells, not the first emitter") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><PhysicalDescriptions><Emitters>
        <Emitter Name="Red" Color="0.7,0.3,20"/></Emitters></PhysicalDescriptions><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1" Geometry="A"><LogicalChannel><ChannelFunction Attribute="ColorAdd_G"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="B"><LogicalChannel><ChannelFunction Attribute="ColorAdd_G"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="B"><LogicalChannel><ChannelFunction Attribute="ColorAdd_B"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const auto a = decode(t, *t.mode("m"), frame_at(1, {255, 255, 255}), 1);
    REQUIRE(a.cells.size() == 2);
    CHECK(a.colour == Rgb{0, 1, 0.5f});
    CHECK(decode(t, *t.mode("m"), frame_at(1, {0, 0, 0}), 1).colour == Rgb{0, 0, 0});
}

TEST_CASE("FR-10 a type with no Dimmer lights fully only when it has a Beam (hazer stays dark)") {
    const auto hazer = parse_description(R"(<GDTF><FixtureType Name="Hazer"><Geometries><Geometry Name="Body"/>
        </Geometries><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Body"><LogicalChannel><ChannelFunction Attribute="Haze1"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Body"><LogicalChannel><ChannelFunction Attribute="Blower1"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    CHECK_FALSE(hazer.has_beam);
    CHECK(decode(hazer, *hazer.mode("m"), frame_at(1, {255, 255}), 1).dimmer == 0);
    CHECK(decode(hazer, *hazer.mode("m"), frame_at(1, {255, 255}), 1).haze == Catch::Approx(1)); // F1: it hazes
    CHECK(decode(hazer, *hazer.mode("m"), frame_at(1, {0, 255}), 1).haze == 0);
    const auto blinder = parse_description(R"(<GDTF><FixtureType Name="Blinder"><Geometries><Geometry Name="Body">
        <Beam Name="Beam"/></Geometry></Geometries><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Beam"><LogicalChannel><ChannelFunction Attribute="ColorAdd_WW"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    CHECK(blinder.has_beam);
    CHECK(decode(blinder, *blinder.mode("m"), frame_at(1, {255}), 1).dimmer == 1);
}

TEST_CASE("FR-14 a fixture with two yokes has two heads, channels and references assigned") {
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><Geometries><Geometry Name="Base">
        <Axis Name="Yoke 1" Position="{1,0,0,-0.2}{0,1,0,0}{0,0,1,0}{0,0,0,1}"><Axis Name="Head 1">
          <GeometryReference Name="Px 1" Geometry="Pixel"><Break DMXBreak="1" DMXOffset="6"/></GeometryReference></Axis></Axis>
        <Axis Name="Yoke 2" Position="{1,0,0,0.2}{0,1,0,0}{0,0,1,0}{0,0,0,1}"><Axis Name="Head 2">
          <GeometryReference Name="Px 2" Geometry="Pixel"><Break DMXBreak="1" DMXOffset="8"/></GeometryReference></Axis></Axis>
        </Geometry><Geometry Name="Pixel"><Beam Name="LED"/></Geometry></Geometries>
        <DMXModes><DMXMode Name="m" Geometry="Base"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Base"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Yoke 1"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="Head 1"><LogicalChannel><ChannelFunction Attribute="Tilt"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="4" Geometry="Yoke 2"><LogicalChannel><ChannelFunction Attribute="Pan" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="5" Geometry="Head 2"><LogicalChannel><ChannelFunction Attribute="Tilt"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="1" Geometry="Pixel" DMXBreak="1"><LogicalChannel><ChannelFunction Attribute="ColorAdd_R"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Pixel" DMXBreak="1"><LogicalChannel><ChannelFunction Attribute="ColorAdd_B"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    const Mode &m = *t.mode("m");
    REQUIRE(m.heads.size() == 2);
    CHECK(m.heads[0].offset[0] == Approx(-0.2));
    CHECK(m.heads[1].offset[0] == Approx(0.2));
    for (const Channel &ch : m.channels) {
        const int want = ch.geometry == "Base" ? -1 : ch.geometry.back() == '1' ? 0 : 1;
        CHECK(ch.head == want);
    }
    const auto a = decode(t, m, frame_at(1, {255, 0, 0, 255, 0, 255, 0, 0, 255}), 1);
    REQUIRE(a.heads.size() == 2);
    CHECK(a.heads[0].pan == Approx(-270));
    CHECK(a.heads[1].pan == Approx(270));
    CHECK(a.heads[0].colour == Rgb{1, 0, 0});
    CHECK(a.heads[1].colour == Rgb{0, 0, 1});

    const auto single = parse_description(R"(<GDTF><FixtureType Name="S"><Geometries><Geometry Name="Base">
        <Axis Name="Yoke"><Axis Name="Head"><Beam Name="B"/></Axis></Axis></Geometry></Geometries>
        <DMXModes><DMXMode Name="m" Geometry="Base"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Yoke"><LogicalChannel><ChannelFunction Attribute="Pan"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Head"><LogicalChannel><ChannelFunction Attribute="Tilt"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    CHECK(single.mode("m")->heads.size() == 1);
    CHECK(decode(single, *single.mode("m"), frame_at(1, {255, 255}), 1).heads.empty());
}

TEST_CASE("FR-14 beam direction composes the geometry rotations down to the first Beam") {
    // Two -45 deg turns about X (Array, then Head), like the TORNADO: the beam leaves horizontally toward +Z.
    const auto t = parse_description(R"(<GDTF><FixtureType Name="X"><Geometries>
        <Geometry Name="Other" Position="{1,0,0,0}{0,-1,0,0}{0,0,-1,0}{0,0,0,1}"/>
        <Geometry Name="Base"><Geometry Name="Array" Position="{1,0,0,0}{0,0.7071068,0.7071068,0}{0,-0.7071068,0.7071068,0}{0,0,0,1}">
          <Geometry Name="Head" Position="{1,0,0,0}{0,0.7071068,0.7071068,0}{0,-0.7071068,0.7071068,0.1}{0,0,0,1}">
            <Beam Name="Beam" LuminousFlux="1000"/></Geometry></Geometry></Geometry>
      </Geometries><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    CHECK(t.beam_direction[0] == Approx(0).margin(1e-6));
    CHECK(t.beam_direction[1] == Approx(0).margin(1e-6));
    CHECK(t.beam_direction[2] == Approx(1));
    CHECK(t.luminous_flux == Approx(1000));

    const auto plain = parse_description(R"(<GDTF><FixtureType Name="X"><Geometries>
        <Geometry Name="Base"><Beam Name="Beam"/></Geometry></Geometries><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="1"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    CHECK(plain.beam_direction == std::array<double, 3>{0, -1, 0});
}

TEST_CASE("Malformed GDTF is rejected with GdtfError") {
    CHECK_THROWS_AS(parse_description("not xml <"), GdtfError);
    CHECK_THROWS_AS(parse_description("<GDTF/>"), GdtfError);
    CHECK_THROWS_AS(parse_description(R"(<GDTF><FixtureType Name="X"><DMXModes><DMXMode Name="m"><DMXChannels>
        <DMXChannel Offset="999"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)"),
                    GdtfError);
    const std::vector<std::uint8_t> junk(64, 7);
    CHECK_THROWS_AS(load_gdtf(junk), GdtfError);
}

TEST_CASE("CIE xy white point maps to white") {
    const auto w = cie_to_rgb(0.3127, 0.3290);
    CHECK(w[0] == Approx(1).margin(0.01));
    CHECK(w[1] == Approx(1).margin(0.01));
    CHECK(w[2] == Approx(1).margin(0.01));
}

#include "gdtf/match.h"

TEST_CASE("FR-22: MVR and CSV type text finds library types and modes by name, numbers and footprint") {
    const auto type = [](const char *maker, const char *name,
                         std::initializer_list<std::pair<const char *, int>> modes) {
        std::string xml =
            std::string(R"(<GDTF><FixtureType Manufacturer=")") + maker + R"(" Name=")" + name + R"("><DMXModes>)";
        for (auto [mode, n] : modes) {
            xml += std::string(R"(<DMXMode Name=")") + mode + R"("><DMXChannels>)";
            for (int i = 1; i <= n; ++i)
                xml += R"(<DMXChannel Offset=")" + std::to_string(i) +
                       R"("><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>)";
            xml += "</DMXChannels></DMXMode>";
        }
        return parse_description(xml + "</DMXModes></FixtureType></GDTF>");
    };
    std::map<std::string, FixtureType, std::less<>> lib;
    lib.emplace("Cameo@CLPIXBAR400PRO@0.1.1.gdtf",
                type("Cameo", "CLPIXBAR400PRO", {{"10-CH-Mode", 10}, {"50-CH-Mode", 50}}));
    lib.emplace("Cameo@PIXBAR_650_CPRO.gdtf", type("Cameo", "PIXBAR 650 CPRO", {{"9-CH-Mode", 9}, {"30-CH-Mode", 30}}));
    lib.emplace("Showtec@Blaze.gdtf", type("Showtec", "Stage Blinder FLEX Blaze", {{"Custom", 5}, {"Basic", 4}}));
    lib.emplace("Generic@Dimmer.gdtf", type("Generic", "Dimmer", {{"Dimmer", 1}}));

    CHECK(match_type(lib, "Cameo@CL PixBar 400 Pro.gdtf").name == "Cameo@CLPIXBAR400PRO@0.1.1.gdtf");
    CHECK(match_type(lib, "Cameo@CL PixBar 400 Pro.gdtf").exact);
    const Match guess = match_type(lib, "Cameo@CL PixBar 650 Pro.gdtf");
    CHECK(guess.name == "Cameo@PIXBAR_650_CPRO.gdtf");
    CHECK_FALSE(guess.exact);
    CHECK(match_type(lib, "Cameo@CL PixBar 600 Pro.gdtf").name.empty());   // numbers differ
    CHECK(match_type(lib, "Showtec@CL PixBar 650 Pro.gdtf").name.empty()); // other maker
    CHECK(match_type(lib, "generic@dimmer").name == "Generic@Dimmer.gdtf");
    // A type whose bare name spells "Maker Name" loses to the type with that maker and name.
    lib.emplace("ACME@ ACME TORNADO@Edwin.gdtf", type("ACME", " ACME TORNADO", {{"Mode 1", 41}}));
    lib.emplace("ACME@TORNADO@V1.50.gdtf", type("ACME", "TORNADO", {{"Mode 1", 41}}));
    CHECK(match_type(lib, "ACME@TORNADO.gdtf").name == "ACME@TORNADO@V1.50.gdtf");

    const auto &bar = lib.at("Cameo@CLPIXBAR400PRO@0.1.1.gdtf");
    CHECK(match_mode(bar, "50-CH-Mode").exact);
    CHECK(match_mode(bar, "50 channel").name == "50-CH-Mode");
    CHECK(match_mode(lib.at("Showtec@Blaze.gdtf"), "Basic - 4 channel").name == "Basic");
    CHECK(match_mode(lib.at("Showtec@Blaze.gdtf"), "Custom mode").name == "Custom");
    CHECK(match_mode(lib.at("Generic@Dimmer.gdtf"), "Mode 0").name == "Dimmer");
    CHECK_FALSE(match_mode(lib.at("Generic@Dimmer.gdtf"), "Mode 0").exact);
}

TEST_CASE("FR-4 write_description round-trips through parse_description") {
    using namespace sv::gdtf;
    FixtureType t;
    t.name = "Built Spot";
    t.short_name = "BS";
    t.manufacturer = "Custom";
    t.id = "6c1e1c5e-0000-4000-8000-000000000001";
    t.beam_angle = 14, t.field_angle = 18, t.luminous_flux = 12000;
    t.body = {0.35, 0.5, 0.3};
    t.emitter = {1, 0.5f, 0.25f};
    t.wheels = {{"Colors", {{"Open", {1, 1, 1}}, {"Red", {1, 0, 0}}}}};
    Mode m{"Default", {}, 0};
    m.channels.push_back({{0}, "", 0, {{"Dimmer", "Dimmer", 0, 0, 1, "", {}}}});
    m.channels.push_back({{1, 2}, "", 32768, {{"Pan", "Pan", 0, -270, 270, "", {}}}}); // 16 bit, centred
    m.channels.push_back({{3}, "", 0, {{"Color1", "Color1", 0, 1, 2, "Colors", {{0, 1}, {128, 2}}}}});
    t.modes.push_back(m);

    const FixtureType r = parse_description(write_description(t));
    CHECK(r.name == "Built Spot");
    CHECK(r.short_name == "BS");
    CHECK(r.manufacturer == "Custom");
    CHECK(r.id == t.id);
    CHECK(r.beam_angle == 14);
    CHECK(r.field_angle == 18);
    CHECK(r.luminous_flux == 12000);
    CHECK(r.body == t.body);
    for (int i = 0; i < 3; ++i)
        CHECK(r.emitter[i] == Catch::Approx(t.emitter[i]).margin(0.01));
    REQUIRE(r.modes.size() == 1);
    const Mode &rm = r.modes[0];
    CHECK(rm.footprint == 4);
    REQUIRE(rm.channels.size() == 3);
    CHECK(rm.channels[1].offsets == std::vector<std::uint16_t>{1, 2});
    CHECK(rm.channels[1].default_value == 32768);
    CHECK(rm.channels[1].geometry == "Yoke");
    CHECK(rm.channels[1].functions[0].physical_from == -270);
    CHECK(rm.channels[2].functions[0].wheel == "Colors");
    REQUIRE(rm.channels[2].functions[0].sets.size() == 2);
    CHECK(rm.channels[2].functions[0].sets[1].dmx_from == 128);
    CHECK(rm.channels[2].functions[0].sets[1].wheel_slot == 2);
    REQUIRE(r.wheels.size() == 1);
    CHECK(r.wheels[0].slots[1].colour[0] == Catch::Approx(1));
    CHECK(r.wheels[0].slots[1].colour[1] == Catch::Approx(0).margin(0.01));
}

TEST_CASE("rgb_to_cie inverts cie_to_rgb for the primaries and white") {
    using namespace sv::gdtf;
    for (const Rgb c : {Rgb{1, 0, 0}, Rgb{0, 1, 0}, Rgb{0, 0, 1}, Rgb{1, 1, 1}}) {
        const auto [x, y] = rgb_to_cie(c);
        const Rgb back = cie_to_rgb(x, y);
        for (int i = 0; i < 3; ++i)
            CHECK(back[i] == Catch::Approx(c[i]).margin(0.01));
    }
}

TEST_CASE("continuous_attribute: intensity, position and colour mix fade; wheels, shutter and control switch") {
    using sv::gdtf::continuous_attribute;
    for (const char *a : {"Dimmer", "Pan", "Tilt", "ColorAdd_R", "ColorSub_C", "CTO", "Zoom", "Focus1", "Iris"})
        CHECK(continuous_attribute(a));
    for (const char *a : {"Color1", "Gobo1", "Shutter1", "StrobeFrequency", "Control1", "DimmerMode", "PanRotate"})
        CHECK_FALSE(continuous_attribute(a));
}
