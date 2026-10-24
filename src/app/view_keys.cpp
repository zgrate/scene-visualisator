#include "view_keys.h"

#include <QVariantMap>
#include <algorithm>
#include <array>

namespace sv::app {

namespace {

constexpr auto none = Qt::NoModifier, shift = Qt::ShiftModifier, ctrl = Qt::ControlModifier, alt = Qt::AltModifier;
using A = ViewAction;

// Blender's numpad steps (15°), grandMA 3D's arrows; Ctrl pans in both.
const ViewKey table[] = {
    {Qt::Key_Left, none, false, both, A::orbit, 15, 0, "", "Camera", "← → ↑ ↓", "Orbit 15°"},
    {Qt::Key_Right, none, false, both, A::orbit, -15, 0},
    {Qt::Key_Up, none, false, both, A::orbit, 0, 15},
    {Qt::Key_Down, none, false, both, A::orbit, 0, -15},
    {Qt::Key_Left, shift, false, both, A::orbit, 5, 0, "", "Camera", "Shift + arrows", "Orbit 5°"},
    {Qt::Key_Right, shift, false, both, A::orbit, -5, 0},
    {Qt::Key_Up, shift, false, both, A::orbit, 0, 5},
    {Qt::Key_Down, shift, false, both, A::orbit, 0, -5},
    {Qt::Key_Left, ctrl, false, both, A::pan, 0.1f, 0, "", "Camera", "Ctrl + arrows", "Pan"},
    {Qt::Key_Right, ctrl, false, both, A::pan, -0.1f, 0},
    {Qt::Key_Up, ctrl, false, both, A::pan, 0, 0.1f},
    {Qt::Key_Down, ctrl, false, both, A::pan, 0, -0.1f},
    {Qt::Key_PageUp, none, false, both, A::zoom, 1, 0, "", "Camera", "PgUp PgDn", "Zoom"},
    {Qt::Key_PageDown, none, false, both, A::zoom, -1, 0},
    {Qt::Key_4, none, true, both, A::orbit, 15, 0, "", "Camera", "Num 4 6 8 2", "Orbit 15°"},
    {Qt::Key_6, none, true, both, A::orbit, -15, 0},
    {Qt::Key_8, none, true, both, A::orbit, 0, 15},
    {Qt::Key_2, none, true, both, A::orbit, 0, -15},
    {Qt::Key_4, ctrl, true, both, A::pan, 0.1f, 0, "", "Camera", "Ctrl + Num 4 6 8 2", "Pan"},
    {Qt::Key_6, ctrl, true, both, A::pan, -0.1f, 0},
    {Qt::Key_8, ctrl, true, both, A::pan, 0, 0.1f},
    {Qt::Key_2, ctrl, true, both, A::pan, 0, -0.1f},
    {Qt::Key_Plus, none, true, both, A::zoom, 1, 0, "", "Camera", "Num + −", "Zoom"},
    {Qt::Key_Minus, none, true, both, A::zoom, -1, 0},
    {Qt::Key_7, none, true, both, A::preset, 0, 0, "Top", "View", "Num 7  Ctrl+Num 7", "Top, bottom"},
    {Qt::Key_7, ctrl, true, both, A::preset, 0, 0, "Bottom"},
    {Qt::Key_1, none, true, both, A::preset, 0, 0, "Front", "View", "Num 1  Ctrl+Num 1", "Front, back"},
    {Qt::Key_1, ctrl, true, both, A::preset, 0, 0, "Back"},
    {Qt::Key_3, none, true, both, A::preset, 0, 0, "Side", "View", "Num 3  Ctrl+Num 3", "Side, other side"},
    {Qt::Key_3, ctrl, true, both, A::preset, 0, 0, "Other side"},
    {Qt::Key_0, none, true, both, A::preset, 0, 0, "Perspective", "View", "Num 0", "Perspective"},
    {Qt::Key_9, none, true, both, A::flip, 0, 0, "", "View", "Num 9", "Opposite side"},
    {Qt::Key_Period, none, true, blender, A::frame, 0, 0, "", "View", "Num .", "Frame selection"},
    {Qt::Key_Comma, none, true, blender, A::frame}, // numpad decimal on comma layouts
    {Qt::Key_F, none, false, maya, A::frame, 0, 0, "", "View", "F", "Frame selection"},
    {Qt::Key_Home, none, false, both, A::frame_all, 0, 0, "", "View", "Home", "Frame everything"},
    {Qt::Key_A, none, false, maya, A::frame_all, 0, 0, "", "View", "A", "Frame everything"},
    {Qt::Key_AsciiTilde, shift, false, both, A::walk, 0, 0, "", "Fly", "Shift + `", "Walk on / off"},
    {Qt::Key_QuoteLeft, shift, false, both, A::walk},
    {Qt::Key_G, none, false, blender, A::transform, 0, 0, "move", "Edit", "G R S", "Move, rotate, scale"},
    {Qt::Key_R, none, false, blender, A::transform, 0, 0, "rotate"},
    {Qt::Key_S, none, false, blender, A::transform, 0, 0, "scale"},
    {Qt::Key_X, none, false, blender, A::remove, 0, 0, "", "Edit", "X", "Delete"},
    {Qt::Key_D, shift, false, blender, A::duplicate, 0, 0, "", "Edit", "Shift + D", "Duplicate and move"},
    {Qt::Key_A, none, false, blender, A::select_all, 0, 0, "", "Edit", "A  Alt+A", "Select all, none"},
    {Qt::Key_A, alt, false, blender, A::select_none},
    {Qt::Key_W, none, false, maya, A::gizmo, 0, 0, "move", "Edit", "W E R Q", "Move, rotate, scale, all handles"},
    {Qt::Key_E, none, false, maya, A::gizmo, 0, 0, "rotate"},
    {Qt::Key_R, none, false, maya, A::gizmo, 0, 0, "scale"},
    {Qt::Key_Q, none, false, maya, A::gizmo, 0, 0, "all"},
    {Qt::Key_A, ctrl, false, maya, A::select_all, 0, 0, "", "Edit", "Ctrl + A", "Select all"},
};

// Rows the table cannot hold: keys ViewportItem reads while flying or in a modal transform, and window shortcuts.
const std::array<std::array<const char *, 3>, 13> extra{{
    {"Fly", "Hold right mouse", "Fly while held"},
    {"Fly", "W A S D  Q E", "Forward, left, back, right, down, up"},
    {"Fly", "Mouse", "Look"},
    {"Fly", "Wheel", "Speed"},
    {"Fly", "Shift  Alt", "Fast, slow"},
    {"Fly", "Enter  Esc", "Keep the view, go back (walk)"},
    {"Modal", "X Y Z", "Lock an axis"},
    {"Modal", "0–9 . −", "Type a value"},
    {"Modal", "Enter  Esc", "Confirm, cancel"},
    {"Edit", "Alt + arrows  Alt+PgUp PgDn", "Nudge 0.25 m"},
    {"Edit", "Delete", "Delete"},
    {"Edit", "Ctrl + Z  Ctrl+Shift+Z", "Undo, redo"},
    {"Edit", "Esc", "Clear the selection"},
}};

int emulated(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return key;
    return key == Qt::Key_Minus ? Qt::Key_Minus : key == Qt::Key_Equal ? Qt::Key_Plus : 0;
}

} // namespace

const ViewKey *find_view_key(int key, Qt::KeyboardModifiers mods, bool keypad, bool blender, bool emulate) {
    mods &= shift | ctrl | alt;
    if (!keypad && emulate && emulated(key))
        key = emulated(key), keypad = true;
    // Shift+` arrives as ~ on most layouts; Shift+numpad keys are not used. macOS flags arrows and Home as keypad keys,
    // so only numpad entries insist on the flag.
    for (const ViewKey &k : table)
        if (k.key == key && k.mods == mods && (k.keypad == keypad || !k.keypad) && k.presets & (blender ? Presets::blender : maya))
            return &k;
    return nullptr;
}

QVariantList view_key_help(bool blender) {
    QVariantList rows;
    for (const ViewKey &k : table)
        if (*k.keys && k.presets & (blender ? Presets::blender : maya))
            rows.append(QVariantMap{{"group", k.group}, {"keys", k.keys}, {"label", k.label}});
    for (const auto &[group, keys, label] : extra)
        rows.append(QVariantMap{{"group", group}, {"keys", keys}, {"label", label}});
    static const QStringList order{"Camera", "View", "Fly", "Edit", "Modal"};
    std::ranges::stable_sort(rows, {}, [](const QVariant &r) { return order.indexOf(r.toMap()["group"].toString()); });
    return rows;
}

} // namespace sv::app
