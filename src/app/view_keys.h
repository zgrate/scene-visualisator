#pragma once
// The 3D view's keys: one table drives both ViewportItem::keyPressEvent and the keyboard card, so they cannot drift.
// Modal G/R/S keys and fly keys (WASD QE) are handled in ViewportItem and listed here for the card only.
#include <QVariantList>
#include <Qt>

namespace sv::app {

enum class ViewAction {
    orbit,       // arg: degrees, x = yaw, y = pitch
    pan,         // arg: viewport fractions
    zoom,        // arg: steps, positive = closer
    preset,      // name
    flip,        // the opposite side
    frame,       // the selection, or everything when nothing is selected
    frame_all,   //
    walk,        // toggle walk navigation
    transform,   // name: move, rotate or scale (modal)
    remove,      // delete the selection
    duplicate,   // duplicate and move
    select_all,  //
    select_none, //
    gizmo,       // name: all, move, rotate or scale
};

enum Presets { blender = 1, maya = 2, both = 3 };

struct ViewKey {
    int key;
    Qt::KeyboardModifiers mods;
    bool keypad;
    int presets;
    ViewAction action;
    float x = 0, y = 0;                              // orbit, pan or zoom amounts
    const char *name = "";                           // preset, transform or gizmo name
    const char *group = "", *keys = "", *label = ""; // the card; empty keys: covered by the row before
};

// The entry for a key press, or null. `keypad` is Qt::KeypadModifier; `emulate` maps the top-row digits, - and = onto
// the numpad (Blender's "Emulate Numpad").
const ViewKey *find_view_key(int key, Qt::KeyboardModifiers mods, bool keypad, bool blender, bool emulate);
// {group, keys, label} rows for the keyboard card, the preset's table entries then the fly and modal keys.
QVariantList view_key_help(bool blender);

} // namespace sv::app
