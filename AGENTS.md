# AGENTS.md — Scene Visualiser

## 1. What this is
A native desktop previsualiser for stage lighting and LED walls, in the style of Capture. It receives DMX over Art-Net
and sACN, decodes fixtures from GDTF, and renders beams and walls on the GPU. It is C++20 with Qt 6.12 Quick and QRhi.
- **Spec:** https://artifacts.zgrate.ovh/a/KGhqn7zy8Lwo. The local source is
  `.artifacts/fragments/scene-visualiser/body.html` (product docs: FR/NFR, protocols, `.svproj`, acceptance tests,
  roadmap) and `mockups.html` (design 2, "Studio").
- **Current target:** the roadmap's **Beta** row (alpha shipped as M0–M6), Studio design, **dark** palette.
  The Beta plan (milestones B0–B8, decisions B1–B12) is `docs/beta-plan.md`; its ground truth for Resolume is the
  user's Arena 7.17 file in `tests/fixtures/resolume/`.
- **The POC's JavaScript is not product code.** Use `app.js` only as a behaviour reference.
- **The full plan** (milestones M0–M6) is in `docs/alpha-plan.md`.

## 2. Build & test
Linux is the daily dev build. Windows (MSVC) and macOS build in CI on every push.
```sh
export PATH="$HOME/.local/bin:$PATH"                 # pip cmake>=3.28, ninja, clang-format, clangd
export QT_ROOT_DIR=$HOME/Qt/6.12.0/gcc_64            # same variable install-qt-action sets in CI
export SV_EXTRA_PREFIX=$HOME/.local/gldev/root/usr   # GL + xkbcommon dev headers unpacked without sudo
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
tools/screenshot.sh /tmp/shot.png                    # real window on Xvfb + llvmpipe, saved by the app itself
python3 tools/dmx_send.py --proto sacn --universes 1-4 --pattern chase   # test DMX source (stdlib only)
```
- **Claude Code:** `.claude/settings.local.json` sets both environment variables, which are machine-specific and gitignored.
- **Outputs:** binaries land in `build/<preset>/bin/`.
- **`--screenshot <png>`** grabs the window after the first frame, then exits with 0 on success.
- **CI** (`.github/workflows/ci.yml`) runs ubuntu-24.04, windows-2022 and macos-14. Each runs the tests and a smoke
  screenshot, uploaded as an artifact.
- **Windows cross build:** `win-mingw` is a local cross-compile preset. It needs `QT_ROOT_DIR` set to Qt's `mingw_64`
  and `QT_HOST_PATH` set to `gcc_64`.
- **Golden render:** `render_golden` (ctest, under `xvfb-run`) renders the demo rig at 640×360 and compares it with
  `tests/golden/demo.png`. After an intended visual change, regenerate with
  `SV_UPDATE_GOLDEN=1 ctest --preset linux-debug -R render_golden` and look at the PNG before committing it.
- **Demo DMX:** `--generator rainbow|chase|sweep|full` runs an internal pattern, so screenshots show beams.
- **No GPU here:** this machine renders with Mesa llvmpipe. Frame-rate numbers from it are trends, never targets.

## 3. Layout & boundaries
| Target | Path | May use |
|---|---|---|
| `sv_core` | `src/core` | std, nlohmann_json, miniz. **No Qt.** Model, `.svproj`, undo, exporters, OBJ |
| `sv_gdtf` | `src/gdtf` | std, pugixml, miniz. **No Qt.** GDTF reader, DMX→attribute decoder |
| `sv_proto` | `src/protocol` | std, Asio (private). **No Qt.** Art-Net/sACN receivers, merger, triple buffer, generators |
| `sv_render` | `src/render` | Qt Gui + QRhi only (no Quick/QML). Scene, beam and wall passes, shaders via `qt_add_shaders` |
| `SceneVisualiser` | `src/app` | Qt Quick/QML. `ViewportItem` (QQuickRhiItem), models for QML, `qml/` |

Hard rules:
- **Threads:** there are three. The UI thread runs QML and document edits. The render thread is Qt Quick's threaded loop.
  The network thread runs the Asio `io_context`.
- **No DMX work on the UI thread.** The network thread hands frames to the render thread through a per-universe
  `TripleBuffer`, latest frame wins. The UI meter reads a mutex-guarded copy at 20 Hz.
- **The renderer reads decoded attributes,** never raw DMX.
- **Every document edit goes through `Document::apply(json_patch)`.** Undo stores the inverse patch. There are no
  per-command classes.
- **Internal units are metres and degrees,** Y-up, with Z toward the audience. Convert only at file boundaries.

## 4. Conventions
- **Formatting:** C++20, formatted by `.clang-format` (LLVM base, 4 spaces, 120 columns). A hook runs it after every
  edit.
- **Names:** files are `snake_case`, types `PascalCase`, functions and variables `snake_case`, namespaces
  `sv::<lib>`.
- **Dependencies:** they come through FetchContent in `cmake/deps.cmake`, pinned by tag. Add one only in the milestone
  that first uses it.
- **Shortcuts:** a known limit gets a `// shortcut: <limit>, <when to upgrade>` comment.
- **Tests:** every new parser, branch-heavy function or protocol path gets a Catch2 test in `tests/`. Exporters use
  approval files in `tests/approved/`. Render checks use golden PNGs in `tests/golden/`.
- **Qt keywords:** `QT_NO_KEYWORDS` is set project-wide because our structs use `slots`. In Qt code write
  `Q_SIGNALS`, `Q_SLOTS` and `Q_EMIT`.
- **Commits:** cite spec ids in commits and test names, for example `FR-9`, `NFR-4` or `AT-6`.

## 5. Spec references
- **Requirements:** FR-1..28 are functional and NFR-1..12 non-functional. Acceptance tests are AT-1..7. All are in
  `body.html`.
- **Art-Net 4:** UDP 6454. The Port-Address is 15 bits (Net 7, Sub-Net 4, Universe 4). Supported packets are ArtPoll,
  ArtPollReply, ArtDmx and ArtSync. Out-of-order sequence numbers are dropped. The timeout is 3 s.
- **sACN E1.31:** UDP 5568, multicast `239.255.U_hi.U_lo`. It covers priority 0–200, per-address priority (start code
  0xDD), sync, and a 2.5 s timeout.
- **Universe numbering:** project universe U is 1-based and equals sACN universe U. Art-Net Port-Address is U−1, so
  universe 1 is Art-Net 0:0:0. Convert only with `universe_from_port_address` and `port_address_from_universe`.
- **Merge:** each universe merges by HTP, LTP or priority (default). Under priority, the highest priority wins; on a tie,
  intensity slots merge HTP and the rest LTP. Internal generators and pasted frames are just more sources.
- **`.svproj`:** a zip of `project.json` (with `schemaVersion`) plus `gdtf/`, `geometry/`, `media/` and `exports/`.
  Writes are atomic (temp file then rename), with 3 backups and a 2-minute autosave. Every object has a UUID. Unknown
  keys are preserved.

## 6. Design rules (Studio, dark)
- **Layout:** the 3D view is the page, with a 36 px icon rail on the left (Design, Patch, DMX monitor, Mapping, Project).
  Floating cards sit on top: Objects at top-left, Inspector at top-right. They can be dragged and collapsed, and their
  positions persist in the project's `layout`.
- **Dock:** a bottom dock holds the 512-bar universe meter. It is a C++ `QQuickItem` with one geometry node, never a
  Repeater. The patch sheet reuses it with `occupancy: true`.
- **Screens:** Patch sheet (`PatchScreen.qml`), DMX monitor (`DmxScreen.qml`, grid is the C++ `ChannelGrid`) Mapping
  (`MappingScreen.qml`) and Project (`ProjectScreen.qml`) cover the viewport, which keeps running underneath. `--screen patch|dmx|mapping|project` starts on one,
  which is how screenshots of them are taken.
- **Controls:** use `Input` and `Check` (Studio-styled TextField and CheckBox) for free-standing fields; stock Fusion
  ones vanish on dark panels. Inspector rows use `Field`.
- **Title bar:** the project name (• when unsaved), the tabs Design/Patch/DMX/Mapping, and the
  Art-Net/sACN status.
- **Colours:** use the `Theme` QML singleton only (`src/app/qml/Theme.qml`). Components contain no colour literals.
  Fusion style; its palette is set from Theme tokens in `Main.qml` (`QGuiApplication::setPalette` does not reach Quick
  Controls here).

| Token | Value | Token | Value |
|---|---|---|---|
| bg | `#121418` | accent | `#4c8dff` |
| panel | `#1b1e24` | accentOn | `#ffffff` |
| line | `#2c3039` | accentSoft | `#1d2f4d` |
| fg | `#e8e6e1` | grid / deck / truss | `#262a33` / `#23262e` / `#a8adb8` |
| muted | `#a09d95` | ok / warn / err | `#3ddc97` / `#ffd166` / `#ff6b6b` |
| viewport | `#0b0c10` | | |

- **Cards:** a 1 px `line` border, radius 8, and a soft dark shadow.
- **Type:** monospace for numbers, system sans for labels, and 8 px uppercase tracked section headers.
- **Selection:** uses the accent colour.
- **Keyboard:** Ctrl (Cmd on macOS) with Z or Shift+Z for undo and redo, D duplicates, O/S open and save. Delete
  removes the selection, arrows nudge it 0.25 m (Page Up/Down for height), Escape clears it.
- **Viewport mouse:** left drag orbits, middle or Shift drag pans, wheel zooms, click selects (Ctrl adds). Left drag on a
  selected object moves it on its height plane, snapped to 0.25 m (Alt: free).

## 7. Agent tooling
- **Skills** (qt-development-skills plugin): use `/qt-cmake-project` for CMake and `/qt-qml` for QML. Use
  `/qt-qml-review` or `/qt-cpp-review` before finishing UI or C++ work, and `/qt-qml-test` for QML tests. The plugin's
  Qt docs MCP handles API lookups.
- **Reviewer agents** (qt-suite plugin): `qt-app-reviewer`, `qt-ux-advisor` and `qt-debugger`. Its Qt Pilot MCP only
  drives Python apps, so do not use it here.
- **clangd-lsp:** needs `clangd` on PATH (pip, `~/.local/bin`). `.clangd` points it at `build/linux-debug`.
- **Reviews and commits:** use `/code-review` before merges, and commit-commands for commits and PRs.
- **Driving the real app:** the `gui-user` MCP (`.mcp.json`) does this under Xvfb. Launch the app through
  `tools/a11y-launch.sh`, never the raw binary, or no controls are visible.
- **Library docs:** use the context7 MCP for Asio, Catch2, nlohmann_json and pugixml.
- **Hooks** (`.claude/settings.json`): clang-format runs after edits. Build and ctest run before Claude stops, and a
  failure blocks the stop.

## 8. Decisions (do not re-litigate without the user)
- **D0:** toolchain from pip in user space (cmake, ninja, aqtinstall, clang-format, clangd). There is no sudo here.
- **D1:** Linux for daily dev, Windows and macOS through CI. NFR-2 frame rate is measured on a real GPU laptop.
- **D2:** Qt 6.12.0 LTS (fallback 6.8.3 LTS).
- **D3:** the project is LGPL-3.0-or-later (`COPYING.LESSER` + `COPYING`). Qt under LGPL v3 with dynamic linking,
  confirmed by the user on 2026-10-22. Ship Qt as replaceable shared
  libraries, never statically linked. Use only modules offered under LGPL; check `$QT_ROOT_DIR/sbom/*.spdx` before
  adding one. Build tools such as qsb and moc are GPL with Qt's exception, so their output carries no obligation.
- **D4:** FetchContent for the alpha, not vcpkg. Switch to a vcpkg manifest in Beta (NDI, Spout, FBX). The plan's
  spdlog and tinyobjloader were not needed: the OBJ reader is in `core/interchange` and the log is a Qt message handler.
- **D5:** our own GDTF reader on pugixml and miniz, not libMVRgdtf (licence and Boost/Xerces). No GDTF 3D models
  in alpha.
- **D6:** built-in meshes for fixtures, trusses and walls. OBJ import produces static set pieces.
- **D7:** Qt Quick/QML, not Widgets.

- **B1–B12 (Beta):** see `docs/beta-plan.md`. In short: D3D11 pinned on Windows, own binary FBX writer, NDI loaded
  at runtime with discovery off, QtMultimedia for video/capture, own MVR with cgltf, Resolume preset round trip plus
  REST client, FetchContent kept (re-opens D4), built-in `pixelStrip` object for LED strips.
- **Schema 2:** `migrate()` in `core/model.cpp` upgrades schema 1 files on load (`composition.source` becomes
  `{kind,...}`, top-level `layers` and `outputs`).

## 9. Deferred (Release, do not build in Beta)
- **Media:** Spout, Syphon and NDI *output* of the 3D view (optional milestone B8, only on request), MVR-xchange.
- **Rendering:** shadows and emissive wall lighting, Bezier warp preview.
- **Other:** the GDTF Share browser, DMX recorder, render export and headless render node.

## 10. Core API in brief (M1)
- **`core/model.h`:** the document is a `nlohmann::json` tree in project.json form. Factories `new_project()`,
  `new_fixture()`, `new_led_wall()`, `new_truss()`, `new_set_piece()`, `new_camera()` and `new_deck()` define defaults.
  `wall_derived()` gives native resolution and module count (FR-7).
- **`core/document.h`:** `Document::apply(patch, label)` is the only way to edit. Undo, redo, history and dirty
  tracking come with it.
- **`core/project_file.h`:** `load_package` and `save_package` handle `.svproj` with atomic writes and three backups.
  `write_autosave` and `has_newer_autosave` support recovery on launch (AT-6).
- **`core/interchange.h`:** `patch_csv`, `ma3_patch_xml`, `resolume_xml` (exports, byte-checked against
  `tests/approved/`), `resolume_import_patch` (a JSON patch: composition size, walls matched by name), `parse_obj`
  (groups with flat-normal vertices) and `unit_scale`. Errors throw `InterchangeError`. To re-approve an export, check
  the `.received` file the test writes, then move it over the approved one.
- **`nlohmann` gotcha:** `json::value(key, default)` returns a temporary. Never keep a pointer or reference into it;
  iterate `at(key)` behind a `contains` check instead.

## 11. DMX API in brief (M2)
- **`protocol/dmx_engine.h`:** `DmxEngine` owns the network thread. It handles Art-Net (ArtDmx, ArtSync hold, ArtPoll
  reply) and sACN (own E1.31 receiver: priority, 0xDD, sync, termination). Call `subscribe()` with at most 64 universes.
  The render thread reads `frames(u)`, a `TripleBuffer<UniverseFrame>` with one consumer per universe. The UI polls
  `status()` for data, rate, loss and sources.
- **Internal sources:** `set_internal_frame()` handles paste-a-frame and the fader page. `set_generator()` runs at 44 Hz.
  Generator targets map GDTF attribute names (`Dimmer`, `Pan`, `Tilt`, `ColorAdd_R/G/B/W`, `Color1`) to footprint
  offsets.
- **Packets and merge:** `protocol/packets.h` builds and parses packets, also for tests. `protocol/merger.h` holds the
  merge rule.
- **We wrote our own sACN receiver, not the ETCLabs library.** This is the spec's named fallback: one socket model and
  no EtcPal.

## 12. GDTF API in brief (M3)
- **`gdtf/fixture_type.h`:** `load_gdtf(path or bytes)` and `parse_description(xml)` produce a `FixtureType`. It
  holds modes, channels with coarse-first offsets, channel functions with physical ranges, ChannelSets, wheels with
  slot colours, the emitter colour, the first beam's angles and flux, and the body size. Errors throw `GdtfError`.
- **`gdtf/decoder.h`:** `decode(type, mode, universe_frame, address)` returns `Attributes`: dimmer, pan/tilt in
  degrees, beam angle, focus, iris, frost, colour, wheel slots, strobe and pixel `cells`. `intensity_offsets(mode)`
  feeds the merger's HTP mask.
- **Bundled generic types:** five live in `resources/gdtf/*.gdtf`: Spot 7ch, Beam 5ch, Wash RGBW, Blinder and Pixel
  Bar 8. They are generated by `tools/make_generic_gdtf.py`; edit the script, never the zips.
- **Attribute names:** use the GDTF names everywhere (`Dimmer`, `Pan`, `Tilt`, `ColorAdd_R`, `Color1`, `Gobo1`,
  `Zoom`, `Shutter1`). Never use the POC's names (`Red`, `Colour`).

## 13. Renderer API in brief (M4)
- **Data flow:** `decode_fixtures(project, library, frame_lookup)` gives a `FixtureState` per fixture uuid.
  `build_scene(project, fixtures, selection, time)` then turns it into a `RenderScene` of plain instance lists (boxes,
  beams, walls, floor spots, pick boxes). `SceneRenderer` draws that list. It knows nothing about JSON or GDTF.
- **`SceneRenderer`:** call `initialize(rhi, rt)` every frame (cheap) and then `render(cb, rt)`. `set_scene`,
  `set_composition(QImage)` and `set_camera(OrbitCamera, time)` only store their input. Uploads happen in `render`.
- **Passes, in order:** grid → boxes → LED walls → beams. Beams are drawn additively, back faces only, without depth
  writes. Their look is analytic: the closest approach of the view ray to the beam axis.
- **Shaders:** Vulkan GLSL 440 in `src/render/shaders/`, compiled by `qt_add_shaders` to `:/sv/shaders/<file>.qsb`.
  Every shader repeats the std140 `Frame` block, which must match `FrameUniforms` (1152 bytes). Never call
  `smoothstep` with edge0 > edge1: it is undefined and llvmpipe renders garbage.
- **Transforms:** object transform is T·Ry·Rx·Rz·S, with parents applied. A fixture beam is local −Y. Pan turns about
  Y, and positive tilt swings the beam toward +Z. A +X rotation of 35° on a front-truss fixture aims it upstage.
- **App side:** `Session` (QML singleton) owns the `Document`, an immutable `snapshot()` for the render thread, the
  `FixtureLibrary` (bundled GDTF from `:/gdtf`), the `DmxEngine` and the selection. `ViewportItem::synchronize`
  copies whatever changed, keyed on `Session::revision()`. `render()` pulls the TripleBuffers and rebuilds the scene.
- **Picking:** CPU only. `pick(scene, ray)` tests unit-cube pick boxes, and `OrbitCamera::ray` gives the ray for a
  viewport point.

## 14. App and QML API in brief (M5)
- **`Session` is the only QML entry point** (singleton, `import SceneVisualiser`). Properties: `objects`, `current`
  (first selected object as JSON plus derived `footprint`, `patchText`, `overflow`, `resolution`), `selectionCount`,
  `fixtureTypes`, `universes`, `layout`, `canUndo`/`canRedo`, `projectName`, `filePath`, `dirty`, and `dmxStatus`
  (refreshed at 20 Hz). All notify through `changed()` except `dmxStatus` (`dmxChanged()`).
- **Edits:** `edit(uuid, pointer, value)` sets one field by JSON pointer relative to the object (`"/position/1"`). It
  uses `replace` for existing paths because JSON-patch `add` inserts into arrays. Doubles from QML become integers
  where the stored field is an integer. Other actions: `setFixtureType`, `setPatch("u.aaa")`, `addFixture/Wall/Truss`
  (next free fixture ID and address), `duplicateSelection`, `deleteSelection`, `nudgeSelection`, `undo`, `redo`.
- **Layout state:** `setLayout(key, value)` writes `layout` (card positions under `cards.<key>`, `cameraBookmarks`)
  outside the undo history via `Document::layout()`.
- **Files:** `newProject`, `openProject(url)`, `saveProject(url or "")`, `importGdtf(url)` return an error string,
  empty on success. Saved packages carry `gdtf/<file>` for every type the rig uses; other entries round-trip as is.
  The app takes an optional `.svproj` path argument and opens the demo rig without one.
- **Render-thread rule:** the render thread only sees `snapshot()` and `library_snapshot()`, both immutable and replaced
  on change. A viewport drag calls `preview_move` (snapshot only) and `commit_move` (one undo step) on release.
- **Singleton gotcha:** `Session(QObject *parent)` has no default argument on purpose. If it is default-constructible,
  QML builds its own instance instead of calling `create()`.
- **Tests:** `sv_session_tests` compiles `session.cpp` directly and runs offscreen. UI checks use `tools/screenshot.sh`
  or `xdotool` under `xvfb-run`; with no window manager there, the window is never active, so `Shortcut`s do not fire.

## 15. Patch, DMX monitor and interchange (M6)
- **Session additions:** `patchRows` (one map per fixture: id, name, type, mode, universe, address, patch, footprint,
  position, conflict, overflow, selected), `conflictCount`, `recoveryFile`. `dmxStatus.perUniverse[u]` also has `lost`
  and `dropped`. C++ only: `occupancy(u)` (512 states: 0 free, 1 used, 2 conflict, 3 selected) and `meshes()`.
- **Actions:** `readdressSelection("u.a")` packs the selection in fixture-ID order, rolling to the next universe.
  `selectUuids(list)`. `exportFile(kind, url)` with kind `csv`, `ma3`, `resolume`, `obj` or `objSelection`.
  `importResolume(url)`, `importObj(url, unit, zUp, split)` (copies the file to `geometry/` in the package and adds
  set pieces). `recover()` and `discardRecovery()`. All return an error string where they can fail.
- **Set pieces:** the renderer draws `MeshLibrary` meshes keyed `geometry/x.obj` or `geometry/x.obj#Group`, double
  sided. A missing mesh shows a red marker box. `export_obj` reuses `build_scene`, one `g` per object, walls as
  `<name>_screen` quads.
- **Autosave (AT-6):** every 2 min to `<file>.autosave`, or `AppDataLocation/Untitled.svproj.autosave` for an untitled
  project. Main.qml offers Recover/Discard when `recoveryFile` is set. Recovery keeps the real path, so the next save
  replaces the real file.
- **Log:** every Qt message also goes to `AppDataLocation/logs/scene-visualiser.log`, rotated at launch (three kept).
- **Release artefacts:** CI packs Windows (windeployqt, zip) and macOS (macdeployqt, dmg) builds, unsigned.

## 16. Resolume and mapping (B3)
- **`core/resolume.h`:** `resolume_import(xml, options)` reads an Arena Advanced Output preset into outputs, walls and
  `pixelStrip`s plus warnings; `resolume_export(project)` writes it back. The source XML stays in
  `project["resolumeSource"]` so untouched elements round-trip byte for byte (`tests/approved/arena-roundtrip.xml`).
- **Params:** every Arena param lives in `obj.resolume.params` under its flat path key (`OutputDevice/.../TargetIP`),
  never re-nested. `Session.resolumeParams(uuid)` lists them with type and range; `setResolumeParam(uuid, key, value)`
  edits one (TargetIP is IPv4 or empty, which stores `TT_DISABLED`). `uuid` may be `"composition"`.
- **Outputs:** `project.outputs` (`kind` is `lumiverse` or a device kind); objects point at one with `output` and map
  through `slice {x, y, w, h, angle}` in composition pixels. `Session.edit` accepts output uuids and `"composition"`.
- **Pixel strips:** `pixelStrip` has `pixels`, `rows`, `colorFormat`, `distribution`, `gamma`, `length`, `patch` and
  `pixelSource` (`dmx` or `slice`). Use `sv::bytes_per_pixel` and `sv::strip_pixel_address`; a strip may span
  universes, so walk channels with `for_each_channel` in `session.cpp`.
- **Send to Resolume:** `resolumePreset()` names `Documents/Resolume Arena/Presets/Advanced Output/<project>.xml`.
- **Mapping screen:** the composition comes from `image://composition/<n>`; slices drag in composition pixels.
