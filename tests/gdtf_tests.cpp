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
