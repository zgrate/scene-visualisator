#!/usr/bin/env python3
"""Writes "GLP@YPOC 700 CMY.gdtf", our own stand-in for the GLP YPOC 700 CMY (GLP publishes no GDTF for it).

Channels and ranges follow the GLP manuals: Basic (24 ch) from the German manual for software 1.28 (instruction
1.15), Extended (33 ch, 16-bit pairs) from the English manual for software 1.53 (instruction 1.33). Zoom 14-32 degrees
from GLP's catalogue. Body size, weight and lumen output are estimates. Run:
  python3 tools/make_ypoc_gdtf.py <out dir>
"""
import pathlib, sys, uuid, zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from make_generic_gdtf import xy  # noqa: E402

# attribute -> (feature, physical unit, activation group); every attribute the channels use is defined.
ATTRS = {
    "Pan": ("Position.PanTilt", "Angle", "PanTilt"), "Tilt": ("Position.PanTilt", "Angle", "PanTilt"),
    "PositionEffect": ("Position.PanTilt", "None", ""), "PositionMSpeed": ("Control.Control", "None", ""),
    "Color1": ("Color.Color", "None", "ColorRGB"), "Color1WheelSpin": ("Color.Color", "AngularSpeed", ""),
    "Color1WheelAudio": ("Color.Color", "None", ""),
    "ColorSub_C": ("Color.RGB", "ColorComponent", "ColorRGB"), "ColorSub_M": ("Color.RGB", "ColorComponent", "ColorRGB"),
    "ColorSub_Y": ("Color.RGB", "ColorComponent", "ColorRGB"), "CTO": ("Color.RGB", "ColorTemperature", "ColorRGB"),
    "Gobo1": ("Gobo.Gobo", "None", "Gobo1"), "Gobo1WheelSpin": ("Gobo.Gobo", "AngularSpeed", "Gobo1"),
    "Gobo1WheelAudio": ("Gobo.Gobo", "None", "Gobo1"), "Gobo1Pos": ("Gobo.Gobo", "Angle", "Gobo1Pos"),
    "Gobo1PosRotate": ("Gobo.Gobo", "AngularSpeed", "Gobo1Pos"), "Gobo1PosAudio": ("Gobo.Gobo", "None", "Gobo1Pos"),
    "Gobo2": ("Gobo.Gobo", "None", "Gobo2"), "Gobo2WheelSpin": ("Gobo.Gobo", "AngularSpeed", "Gobo2"),
    "Gobo2WheelAudio": ("Gobo.Gobo", "None", "Gobo2"),
    "Shutter1": ("Beam.Beam", "None", ""), "Shutter1Strobe": ("Beam.Beam", "Frequency", ""),
    "Shutter1StrobePulse": ("Beam.Beam", "Frequency", ""), "Shutter1StrobeRandom": ("Beam.Beam", "Frequency", ""),
    "Shutter1StrobeEffect": ("Beam.Beam", "None", ""),
    "Dimmer": ("Dimmer.Dimmer", "LuminousIntensity", ""),
    "Focus1": ("Focus.Focus", "None", ""), "Zoom": ("Focus.Focus", "Angle", ""), "Frost1": ("Beam.Beam", "None", ""),
    "Prism1": ("Beam.Beam", "None", "Prism"), "Prism1Pos": ("Beam.Beam", "Angle", "Prism"),
    "Prism1PosRotate": ("Beam.Beam", "AngularSpeed", "Prism"), "Prism1PosAudio": ("Beam.Beam", "None", "Prism"),
    "Iris": ("Beam.Beam", "None", ""), "IrisRandomPulseOpen": ("Beam.Beam", "Frequency", ""),
    "IrisRandomPulseClose": ("Beam.Beam", "Frequency", ""), "IrisPulseOpen": ("Beam.Beam", "Frequency", ""),
    "IrisPulseClose": ("Beam.Beam", "Frequency", ""), "IrisPulse": ("Beam.Beam", "Frequency", ""),
    "IrisStrobe": ("Beam.Beam", "Frequency", ""),
    "Effects1": ("Beam.Beam", "None", "Effects1"), "Effects1Rate": ("Beam.Beam", "AngularSpeed", "Effects1"),
    "Control1": ("Control.Control", "None", ""),
}

# Colour wheel in the order of software 1.53 (1.28 lists it the other way round); filter numbers as printed.
COLOURS = [("Open", "#ffffff"), ("Red 308", "#ff2a1a"), ("CTC 3200-4100", "#ffd9b0"), ("Yellow 601", "#ffe31a"),
           ("Lavender 502", "#9a5cff"), ("Orange 306", "#ff7a10"), ("Pink 312", "#ff4fa8"), ("Green 206", "#2ee85a"),
           ("Lavender 509", "#b77dff")]
GOBO1 = ["Open"] + [f"Gobo {i}" for i in range(2, 9)]   # rotating, indexable
GOBO2 = ["Open"] + [f"Gobo {i}" for i in range(2, 11)]  # fixed


def f(attr, dmx, phys=(0, 1), name=None, wheel=None, sets=(), default=None):
    w = f' Wheel="{wheel}"' if wheel else ""
    d = f' Default="{default}/1"' if default is not None else ""
    body = "".join(f'<ChannelSet Name="{n}" DMXFrom="{v}/1" WheelSlotIndex="{i}"/>' for n, v, i in sets)
    return (f'<ChannelFunction Name="{name or attr}" Attribute="{attr}" OriginalAttribute="" DMXFrom="{dmx}/1"{d} '
            f'PhysicalFrom="{phys[0]}" PhysicalTo="{phys[1]}" RealFade="0"{w}>{body}</ChannelFunction>')


def ch(offset, geometry, *functions):
    """offset is 5 or (5, 6) for 16 bit; the first function's attribute and name make the logical channel."""
    off = ",".join(map(str, offset)) if isinstance(offset, tuple) else str(offset)
    attr = functions[0].split('Attribute="')[1].split('"')[0]
    name = functions[0].split('Name="')[1].split('"')[0]
    return (f'<DMXChannel DMXBreak="1" Offset="{off}" InitialFunction="{geometry}_{attr}.{attr}.{name}" '
            f'Highlight="None" Geometry="{geometry}"><LogicalChannel Attribute="{attr}" Snap="No" Master="None" '
            f'MibFade="0" DMXChangeTimeLimit="0">' + "".join(functions) + "</LogicalChannel></DMXChannel>")


def wheel_sets(names, start, step):
    return [(n, start + i * step, i + 1) for i, n in enumerate(names)]


def spin(prefix, stop=128):
    return [f(f"{prefix}Spin", stop, (0, 0), name="Stop"), f(f"{prefix}Spin", stop + 2, (0.5, 120), name="CW"),
            f(f"{prefix}Spin", 192, (-120, -0.5), name="CCW"), f(f"{prefix}Audio", 254, name="Audio")]


def channels(extended):
    """The fixture's channels in order; extended mode makes the 16-bit attributes two slots wide."""
    n = [1]

    def at(fine):
        o = (n[0], n[0] + 1) if fine and extended else n[0]
        n[0] += 2 if fine and extended else 1
        return o

    colour_names = [c for c, _ in COLOURS[1:]]
    out = [
        ch((1, 2), "Yoke", f("Pan", 0, (-265, 265), default=128)),
        ch((3, 4), "Head", f("Tilt", 0, (-142.5, 142.5), default=128)),
    ]
    n[0] = 5
    out += [
        ch(at(True), "Beam",
           f("Color1", 0, (0, 0), name="Colour fast", wheel="Color1",
             sets=[("Open", 0, 1)] + [(c, 4 + 4 * i, i + 2) for i, c in enumerate(colour_names)] + [("Open", 36, 1)]),
           f("Color1", 64, (0, 0), name="Colour slow", wheel="Color1",
             sets=[("Open", 64, 1)] + [(c, 68 + 4 * i, i + 2) for i, c in enumerate(colour_names)] + [("Open", 100, 1)]),
           *spin("Color1Wheel")),
        ch(at(True), "Beam", f("ColorSub_C", 0)),
        ch(at(True), "Beam", f("ColorSub_M", 0)),
        ch(at(True), "Beam", f("ColorSub_Y", 0)),
        ch(at(False), "Beam",
           f("Gobo1", 0, (0, 0), name="Gobo 1 fast", wheel="Gobo1", sets=wheel_sets(GOBO1, 0, 8)),
           f("Gobo1", 64, (0, 0), name="Gobo 1 slow", wheel="Gobo1", sets=wheel_sets(GOBO1, 64, 8)),
           *spin("Gobo1Wheel")),
        ch(at(True), "Beam", f("Gobo1Pos", 0, (0, 540)), f("Gobo1PosRotate", 128, (0, 0), name="Stop"),
           f("Gobo1PosRotate", 130, (0.03, 228), name="CW"), f("Gobo1PosRotate", 192, (-228, -0.03), name="CCW"),
           f("Gobo1PosAudio", 254, name="Audio")),
        ch(at(False), "Beam",
           f("Gobo2", 0, (0, 0), name="Gobo 2 fast", wheel="Gobo2", sets=wheel_sets(GOBO2, 0, 4)),
           f("Gobo2", 64, (0, 0), name="Gobo 2 slow", wheel="Gobo2", sets=wheel_sets(GOBO2, 64, 4)),
           *spin("Gobo2Wheel")),
        ch(at(False), "Beam", f("Shutter1", 0, (0, 0), name="Closed"),
           f("Shutter1StrobeRandom", 16, (0.7, 10), name="Random strobe"),
           f("Shutter1StrobePulse", 32, (0.7, 10), name="Pulse"), f("Shutter1StrobeEffect", 48, (0, 0), name="Audio"),
           f("Shutter1Strobe", 64, (0.7, 10), name="Strobe"), f("Shutter1", 240, (0, 0), name="Open", default=255)),
        ch(at(True), "Beam", f("Dimmer", 0)),
        ch(at(False), "Beam", f("Focus1", 0)),
        ch(at(False), "Beam", f("Zoom", 0, (14, 32))),
        ch(at(False), "Beam", f("Frost1", 0)),
        ch(at(True), "Beam", f("Prism1", 0, (0, 0), name="Out"), f("Prism1Pos", 6, (0, 540), name="Position"),
           f("Prism1PosRotate", 130, (0.03, 264), name="CW"), f("Prism1PosRotate", 192, (-264, -0.03), name="CCW"),
           f("Prism1PosAudio", 254, name="Audio")),
        ch(at(True), "Beam", f("Iris", 0, (1, 0.1)), f("IrisRandomPulseOpen", 128, (0, 0), name="Open random"),
           f("IrisRandomPulseClose", 144, (0, 0), name="Close random"),
           f("IrisPulseOpen", 160, (0, 0), name="Open audio"), f("IrisPulseClose", 176, (0, 0), name="Close audio"),
           f("IrisPulseOpen", 192, (0.5, 5), name="Open slow-fast"),
           f("IrisPulseClose", 208, (0.5, 5), name="Close slow-fast"), f("IrisPulse", 224, (0.5, 5), name="Pulse"),
           f("IrisStrobe", 240, (0.5, 5), name="Open and close"), f("Iris", 254, (1, 1), name="Open")),
        ch(at(True), "Beam", f("CTO", 0, (7500, 3200))),
        ch(at(False), "Beam", f("Effects1", 0, name="Motion wheel")),
        ch(at(False), "Beam", f("Effects1Rate", 0, (0.1, 60), name="CW"), f("Effects1Rate", 128, (-60, -0.1), name="CCW")),
        ch(at(False), "Beam", f("Control1", 0, (0, 0), name="No function"),
           *[f("Control1", v, (0, 0), name=nm) for v, nm in [
               (16, "Gobo 1 shake 10"), (32, "Gobo 1 shake 20"), (48, "Gobo 1 shake 30"), (64, "Unused"),
               (112, "Colour chase C/C+1"), (128, "Colour chase C/C+2"), (144, "Unused 2"),
               (176, "Audio pan/tilt slow"), (192, "Audio pan/tilt fast"), (208, "Unused 3"),
               (224, "Fans minimum"), (230, "Lamp off"), (250, "Reset")]]),
        ch(at(False), "Base", f("PositionEffect", 0, (0, 0), name="No movement"),
           *[f("PositionEffect", v, (0, 0), name=nm) for v, nm in [
               (1, "Pan"), (32, "Tilt"), (64, "Pan tilt"), (96, "Pan tilt inverse"), (128, "Circle"),
               (160, "Circle inverse"), (192, "Figure eight"), (224, "Random")]]),
        ch(at(False), "Base", f("PositionMSpeed", 0, (0, 0), name="Relative"), f("PositionMSpeed", 16, (0, 1), name="Speed")),
    ]
    return out, n[0] - 1


def wheels():
    def w(name, slots):
        return f'<Wheel Name="{name}">' + "".join(f'<Slot Name="{n}" Color="{xy(c)}"/>' for n, c in slots) + "</Wheel>"
    return (w("Color1", COLOURS) + w("Gobo1", [(n, "#ffffff") for n in GOBO1]) +
            w("Gobo2", [(n, "#ffffff") for n in GOBO2]))


def attribute_definitions():
    groups = sorted({g for _, _, g in ATTRS.values() if g})
    features = {}
    for feat, _, _ in ATTRS.values():
        fg, fe = feat.split(".")
        features.setdefault(fg, set()).add(fe)
    out = "<ActivationGroups>" + "".join(f'<ActivationGroup Name="{g}"/>' for g in groups) + "</ActivationGroups>"
    out += "<FeatureGroups>" + "".join(
        f'<FeatureGroup Name="{fg}" Pretty="{fg}">' + "".join(f'<Feature Name="{fe}"/>' for fe in sorted(fes))
        + "</FeatureGroup>" for fg, fes in sorted(features.items())) + "</FeatureGroups>"
    out += "<Attributes>" + "".join(
        f'<Attribute Name="{a}" Pretty="{a}"' + (f' ActivationGroup="{g}"' if g else "")
        + f' Feature="{feat}" PhysicalUnit="{unit}"/>' for a, (feat, unit, g) in ATTRS.items()) + "</Attributes>"
    return out


def mode(name, extended):
    chans, footprint = channels(extended)
    return f'<DMXMode Name="{name}" Geometry="Base"><DMXChannels>{"".join(chans)}</DMXChannels><Relations/><FTMacros/></DMXMode>', footprint


def description():
    basic, b = mode("Basic", False)
    ext, e = mode("Extended", True)
    assert (b, e) == (24, 33), (b, e)
    beam = ('<Beam Name="Beam" Model="Head" Position="{1,0,0,0}{0,1,0,0}{0,0,1,0}{0,0,0,1}" LampType="Discharge" '
            'PowerConsumption="700" LuminousFlux="14000" ColorTemperature="7500" BeamAngle="14" FieldAngle="18" '
            'BeamRadius="0.07" BeamType="Spot" ColorRenderingIndex="85"/>')
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.2">
<FixtureType Name="YPOC 700 CMY" ShortName="YPOC700" LongName="GLP YPOC 700 CMY" Manufacturer="GLP" Description="Scene Visualiser stand-in built from the GLP manuals (software 1.28 and 1.53); not from GLP" FixtureTypeID="{uuid.uuid5(uuid.NAMESPACE_URL, 'scene-visualiser/GLP/YPOC 700 CMY')}" RefFT="" Thumbnail="" CanHaveChildren="Yes">
<AttributeDefinitions>{attribute_definitions()}</AttributeDefinitions>
<Wheels>{wheels()}</Wheels>
<PhysicalDescriptions><Emitters><Emitter Name="MSR Gold 700" Color="{xy('#f4f6ff')}"/></Emitters></PhysicalDescriptions>
<Models><Model Name="Base" Length="0.43" Width="0.36" Height="0.18" PrimitiveType="Base"/><Model Name="Yoke" Length="0.43" Width="0.12" Height="0.4" PrimitiveType="Yoke"/><Model Name="Head" Length="0.3" Width="0.3" Height="0.5" PrimitiveType="Head"/></Models>
<Geometries><Geometry Name="Base" Model="Base" Position="{{1,0,0,0}}{{0,1,0,0}}{{0,0,1,0}}{{0,0,0,1}}"><Axis Name="Yoke" Model="Yoke" Position="{{1,0,0,0}}{{0,1,0,0}}{{0,0,1,-0.18}}{{0,0,0,1}}"><Axis Name="Head" Model="Head" Position="{{1,0,0,0}}{{0,1,0,0}}{{0,0,1,-0.3}}{{0,0,0,1}}">{beam}</Axis></Axis></Geometry></Geometries>
<DMXModes>{basic}{ext}</DMXModes>
<Revisions><Revision Text="Generated by tools/make_ypoc_gdtf.py" Date="2026-10-22T00:00:00" UserID="0"/></Revisions>
</FixtureType>
</GDTF>
"""


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    path = out / "GLP@YPOC 700 CMY.gdtf"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo("description.xml", (2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, description())
    print(path)


if __name__ == "__main__":
    main()
