#!/usr/bin/env python3
"""Writes the bundled generic GDTF types (resources/gdtf/*.gdtf), modelled on the POC library.

They are the placeholder types for unpatched fixtures and the GDTF test fixtures. Re-run after editing:
  python3 tools/make_generic_gdtf.py
"""
import pathlib, uuid, zipfile

OUT = pathlib.Path(__file__).resolve().parent.parent / "resources" / "gdtf"


def xy(hex_colour):
    """sRGB hex -> CIE xy (GDTF ColorCIE without Y)."""
    def lin(c):
        c /= 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(int(hex_colour[i:i + 2], 16)) for i in (1, 3, 5))
    X = 0.4124 * r + 0.3576 * g + 0.1805 * b
    Y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    Z = 0.0193 * r + 0.1192 * g + 0.9505 * b
    s = X + Y + Z or 1
    return f"{X / s:.6f},{Y / s:.6f},{Y * 100:.6f}"


def fn(attr, phys=(0, 1), dmx_from="0/1", name=None, wheel=None, sets=()):
    w = f' Wheel="{wheel}"' if wheel else ""
    body = "".join(f'<ChannelSet Name="{n}" DMXFrom="{f}/1" WheelSlotIndex="{i}"/>' for n, f, i in sets)
    return (f'<ChannelFunction Name="{name or attr}" Attribute="{attr}" DMXFrom="{dmx_from}" '
            f'PhysicalFrom="{phys[0]}" PhysicalTo="{phys[1]}"{w}>{body}</ChannelFunction>')


def channel(offset, geometry, *functions, default="0/1"):
    return (f'<DMXChannel DMXBreak="1" Offset="{offset}" Default="{default}" Geometry="{geometry}">'
            f'<LogicalChannel Attribute="{functions[0].split("Attribute=")[1].split(chr(34))[1]}">'
            + "".join(functions) + "</LogicalChannel></DMXChannel>")


def wheel(name, slots):
    return f'<Wheel Name="{name}">' + "".join(f'<Slot Name="{n}" Color="{xy(c)}"/>' for n, c in slots) + "</Wheel>"


def fixture(name, short, channels, *, beam=25, field=None, model=(0.3, 0.4, 0.3), wheels="", emitter="#ffffff",
            geometries=None, lumens=10000):
    l, h, w = model
    beam_geo = (f'<Beam Name="Beam" Model="Head" BeamAngle="{beam}" FieldAngle="{field or beam * 1.2:g}" '
                f'LuminousFlux="{lumens}" BeamRadius="0.05" BeamType="Spot"/>')
    geos = geometries or f'<Geometry Name="Base" Model="Body"><Axis Name="Yoke"><Axis Name="Head">{beam_geo}</Axis></Axis></Geometry>'
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.2">
<FixtureType Name="{name}" ShortName="{short}" LongName="Generic {name}" Manufacturer="Generic" Description="Scene Visualiser bundled generic type" FixtureTypeID="{uuid.uuid5(uuid.NAMESPACE_URL, 'scene-visualiser/' + name)}" RefFT="" Thumbnail="">
<AttributeDefinitions/>
<Wheels>{wheels}</Wheels>
<PhysicalDescriptions><Emitters><Emitter Name="LED" Color="{xy(emitter)}"/></Emitters></PhysicalDescriptions>
<Models><Model Name="Body" Length="{l}" Width="{w}" Height="{h}" PrimitiveType="Cube"/><Model Name="Head" Length="{l}" Width="{w}" Height="{h / 2}" PrimitiveType="Cylinder"/></Models>
<Geometries>{geos}</Geometries>
<DMXModes><DMXMode Name="Default" Geometry="Base"><DMXChannels>{"".join(channels)}</DMXChannels><Relations/><FTMacros/></DMXMode></DMXModes>
</FixtureType>
</GDTF>
"""


# POC ranges: pan 540 degrees, tilt 270 degrees, DMX 128 is (almost) centre.
PAN, TILT = fn("Pan", (-270, 270)), fn("Tilt", (-135, 135))
DIM = fn("Dimmer")
COLOURS = [("Open", "#ffffff"), ("Red", "#ff3030"), ("Green", "#30ff50"), ("Blue", "#3060ff"), ("Yellow", "#ffe040"),
           ("Magenta", "#ff40ff"), ("Cyan", "#40f0ff"), ("Orange", "#ff9020")]
GOBOS = [(n, "#ffffff") for n in ["Open", "Dots", "Star", "Ring", "Bars", "Breakup", "Leaves", "Cone"]]

TYPES = {
    "Generic@Spot 7ch": fixture("Spot 7ch", "Spot7", [
        channel(1, "Head", DIM), channel(2, "Yoke", PAN, default="128/1"), channel(3, "Head", TILT, default="128/1"),
        channel(4, "Beam", fn("ColorAdd_R")), channel(5, "Beam", fn("ColorAdd_G")), channel(6, "Beam", fn("ColorAdd_B")),
        channel(7, "Beam", fn("Zoom", (8, 40)))], beam=12),
    "Generic@Beam 5ch": fixture("Beam 5ch", "Beam5", [
        channel(1, "Head", DIM), channel(2, "Yoke", PAN, default="128/1"), channel(3, "Head", TILT, default="128/1"),
        channel(4, "Beam", fn("Color1", (1, 8), wheel="Colors",
                              sets=[(n, i * 32, i + 1) for i, (n, _) in enumerate(COLOURS)])),
        channel(5, "Beam", fn("Gobo1", (1, 8), wheel="Gobos",
                              sets=[(n, i * 32, i + 1) for i, (n, _) in enumerate(GOBOS)]))],
        beam=4, field=5, model=(0.25, 0.45, 0.25), wheels=wheel("Colors", COLOURS) + wheel("Gobos", GOBOS)),
    "Generic@Wash RGBW": fixture("Wash RGBW", "WashRGBW", [
        channel(1, "Beam", DIM), channel(2, "Beam", fn("ColorAdd_R")), channel(3, "Beam", fn("ColorAdd_G")),
        channel(4, "Beam", fn("ColorAdd_B")), channel(5, "Beam", fn("ColorAdd_W"))],
        beam=30, model=(0.25, 0.25, 0.25),
        geometries='<Geometry Name="Base" Model="Body"><Beam Name="Beam" Model="Body" BeamAngle="30" FieldAngle="36" LuminousFlux="6000" BeamRadius="0.1" BeamType="Wash"/></Geometry>'),
    "Generic@Blinder": fixture("Blinder", "Blinder", [channel(1, "Beam", DIM)], beam=40, emitter="#ffc88c",
        model=(0.2, 0.3, 0.4),
        geometries='<Geometry Name="Base" Model="Body"><Beam Name="Beam" Model="Body" BeamAngle="40" FieldAngle="60" LuminousFlux="20000" BeamRadius="0.15" BeamType="Wash"/></Geometry>'),
    # grandMA's "Generic Dimmer": a conventional on a dimmer channel.
    "Generic@Dimmer": fixture("Dimmer", "Dimmer", [channel(1, "Beam", DIM)], beam=26, field=36, emitter="#ffd0a0",
        model=(0.25, 0.25, 0.25), lumens=3000,
        geometries='<Geometry Name="Base" Model="Body"><Beam Name="Beam" Model="Body" BeamAngle="26" FieldAngle="36" LuminousFlux="3000" BeamRadius="0.08" BeamType="Wash"/></Geometry>'),
    "Generic@Pixel Bar 8": fixture("Pixel Bar 8", "PixBar8", [channel(1, "Base", DIM)] + [
        channel(2 + i * 3 + k, f"Pixel{i + 1}", fn(f"ColorAdd_{c}")) for i in range(8) for k, c in enumerate("RGB")],
        beam=25, model=(1.0, 0.1, 0.1),
        geometries='<Geometry Name="Base" Model="Body">' + "".join(
            f'<Beam Name="Pixel{i + 1}" Model="Body" BeamAngle="25" FieldAngle="30" LuminousFlux="800" BeamRadius="0.04" BeamType="Wash"/>'
            for i in range(8)) + "</Geometry>"),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, xml in TYPES.items():
        path = OUT / (name.replace(" ", "_") + ".gdtf")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            info = zipfile.ZipInfo("description.xml", (2026, 1, 1, 0, 0, 0))  # fixed date: reproducible bytes
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, xml)
        print(path.relative_to(OUT.parent.parent))


if __name__ == "__main__":
    main()
