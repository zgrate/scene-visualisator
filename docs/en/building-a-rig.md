**English** · [Polski](../pl/building-a-rig.md) · [Docs home](../../README.md#documentation)

# Building a rig

![Build palette](../images/build.png)

## New project wizard

**New from wizard…** (Project screen, or **New project…** in the palette) opens a wizard. It sets the venue size, the
stage deck, the trusses and a first set of fixtures in one go. You can change everything afterwards.

## The palette

The **Build** rail entry (the bottom icon) opens the palette. Drag a tile onto the stage, or click it to add the
object at the centre.

- **Deck, Box, Cylinder, Wedge, Plane:** stage platforms and simple set pieces.
- **Truss, LED wall, Pixel strip.**
- **Fixture tiles** (Spot 7ch, Wash RGBW, …): one tile per fixture type in the library.
- **DRAW:**
  - **Walls:** click points on the floor (Top view) to build a run of walls of height `h` and thickness `t`.
  - **Deck:** draws a deck of height `h`.
- **CREATE:**
  - **Fixture array…:** a row of fixtures with consecutive IDs and DMX addresses.
  - **Fixture type…:** the fixture type editor (see below).
  - **New project…:** the wizard.

## Moving things

- Select an object, then drag the coloured gizmo handles to move it along an axis, rotate it about Y, or scale it.
  Movement snaps to 0.25 m, rotation to 5° and scale to 0.05. Hold Alt for free movement.
- With the keyboard (Blender preset):
  1. Press G to move, R to rotate or S to scale.
  2. Optionally press X, Y or Z to lock an axis, and type a number for an exact value.
  3. Press Enter to confirm, or Esc to cancel.
- For exact values, type into the Inspector.
- **Hanging fixtures:** set **Parent** to a truss, so the fixtures move with it.
- Units are metres and degrees. Y points up and Z points towards the audience.

## Fixture types

- The app ships six generic types: Spot 7ch, Beam 5ch, Wash RGBW, Blinder, Pixel Bar 8 and Generic Dimmer.
- **Project → IMPORT → GDTF…** (or dropping a `.gdtf` on the window) adds any GDTF file, for example one downloaded
  from [GDTF Share](https://gdtf-share.com).
- **Fixture type…** in the palette opens a form: channels, beam angle, colour mixing, wheels. It saves a GDTF file for
  simple fixtures that have no file yet.
- The project file carries every type the project uses, so it opens the same on another computer.

## Venue

The **VENUE** section on the Project screen sets the room width, depth, height and minimum haze. **Fit to geometry**
sizes the room around everything you have built.

Next: [DMX and patch](dmx-and-patch.md)
