**English** · [Polski](../pl/getting-started.md) · [Docs home](../../README.md#documentation)

# Getting started

## First launch

Scene Visualiser opens a demo rig the first time: a deck, two trusses, spots, washes, beams, blinders, a pixel bar and
three LED walls. To open your own project instead, pass a `.svproj` file, drop one on the window, or use
**Project → Open…**.

If the app closed without saving, it offers to **Recover** the last autosave on the next launch.

## The window

![Design view](../images/hero.png)

- **Title bar:** the project name (a • means unsaved changes), the screen tabs, and the Art-Net and sACN status lights.
  The **3D** switch pauses the viewport to save power.
- **Icon rail (left):** Design, Patch, DMX monitor, Control, Plan, Mapping, Project, and Build (the object palette).
- **Objects card (top left):** every object in the rig. Click to select; Shift or Ctrl adds to the selection.
- **Inspector card (top right):** the fields of the selected object, such as position, rotation, fixture type, mode
  and patch.
- **Dock (bottom):** a 512-channel meter for one universe, the DMX source and rate, **MEDIA** (the composition source)
  and **REC** (recording and playback).

Both cards can be dragged and collapsed. Their positions are saved with the project.

## Moving the camera

Pick a control preset on the **Project** screen: **Blender** (the default) or **Maya**.

| Action | Blender | Maya |
|---|---|---|
| Orbit | middle drag (or Alt + left) | Alt + left |
| Pan | Shift + middle | Alt + middle |
| Zoom | wheel, or Ctrl + middle | Alt + right, or wheel |
| Frame selection | numpad `.` | F |
| Move / rotate / scale | G / R / S | W / E / R |

Both presets share these keys:

| Key | Action |
|---|---|
| Numpad 7 / 1 / 3 / 0 | top / front / side / camera view |
| Arrows | orbit 15° (Shift 5°, Ctrl pans) |
| Home | frame everything |
| Hold right button | fly with WASD, Q and E |
| Shift + ` | walk mode |
| `?` or F1 | show every key |

No numpad? Turn on **Emulate numpad** on the Project screen to use the top-row digits instead.

## Editing

Every change can be undone with Ctrl+Z and redone with Ctrl+Shift+Z.

| Key | Action |
|---|---|
| Ctrl+D | duplicate the selection |
| Delete | remove the selection |
| Alt + arrows | nudge the selection 0.25 m (Alt+Page Up/Down for height) |
| Escape | clear the selection |

## Saving

- **Ctrl+S** saves and **Ctrl+Shift+S** saves as. A `.svproj` file is a zip that holds the project and every GDTF,
  model and media file it uses, so you can share the one file.
- Each save keeps three backups next to the file (`.bak1` to `.bak3`).
- The app autosaves every 2 minutes.
- **RECENT** on the Project screen lists the last 10 files.
- If a project names a file that cannot be found, a dialog lists it when the project opens.

Next: [Building a rig](building-a-rig.md)
