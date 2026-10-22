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
| `sv_core` | `src/core` | std, nlohmann_json, miniz, pugixml, cgltf, ufbx. **No Qt.** Model, `.svproj`, undo, exporters, OBJ, MVR, FBX |
| `sv_gdtf` | `src/gdtf` | std, pugixml, miniz. **No Qt.** GDTF reader, DMX→attribute decoder |
| `sv_proto` | `src/protocol` | std, Asio (private). **No Qt.** Art-Net/sACN receivers, merger, triple buffer, generators |
| `sv_render` | `src/render` | Qt Gui + QRhi only (no Quick/QML). Scene, beam and wall passes, shaders via `qt_add_shaders` |
| `sv_media` | `src/media` | Qt Gui + Multimedia. `MediaSource`: video files, screen and window capture |
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
- **Screens:** Patch sheet (`PatchScreen.qml`), DMX monitor (`DmxScreen.qml`, grid is the C++ `ChannelGrid`) Plan
  (`PlanScreen.qml`), Mapping (`MappingScreen.qml`) and Project (`ProjectScreen.qml`) cover the viewport, which keeps
  running underneath. `--screen patch|dmx|plan|mapping|project` starts on one,
  which is how screenshots of them are taken.
- **Controls:** use `Input` and `Check` (Studio-styled TextField and CheckBox) for free-standing fields; stock Fusion
  ones vanish on dark panels. Inspector rows use `Field`.
- **Title bar:** the project name (• when unsaved), the tabs Design/Patch/DMX/Plan/Mapping, and the
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
- **Release artefacts:** CI packs Windows (windeployqt, zip and MSI) and a universal macOS dmg. See section 22.

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

## 17. Composition sources (B1)
- **`composition.source.kind`:** `testPattern`, `grid` (mapping grid with coordinates), `still`, `video`, `screen` or
  `window`, `ndi`, `spout` or `syphon` (section 21). `make_composition(project, still)` in `render/test_pattern.h` draws patterns, or places `still` by
  `source.fit` (fit, fill, stretch); walls get outlines on top.
- **`MediaSource`** (`media/media_source.h`) lives on the UI thread. Frames land in a mutex-guarded latest-wins slot;
  the render thread polls `latest(serial)`, which returns a null image when nothing is new. The player and the capture
  session each need their own `QVideoSink`: one sink on both delivers nothing.
- **Session:** `setMediaFile(kind, url)` copies files up to 200 MB into `media/` in the package and links larger ones
  by absolute path. `mediaSources(kind)` lists screens, windows or senders. `mediaStatus` is `{kind, width, height, fps, signal, latency,
  live, error}`, refreshed with `dmxChanged()`. The dock's MEDIA button opens `MediaCard.qml`.
- **Screenshots:** `--screenshot` waits up to 5 s for a live source's first frame.

## 18. MVR and CSV (B4)
- **`core/mvr.h`:** `mvr_import(zip)` returns layers, objects, package files and warnings; `mvr_export(project,
  files)` writes MVR 1.6. Objects keep their MVR UUIDs, and unread child elements stay verbatim in `extras.mvr.xml`, so
  import, export and import loses nothing (AT-1). `<AUXData>` round-trips through `project["mvr"]["auxData"]`.
- **Mapping:** Fixture becomes `fixture`, VideoScreen `ledWall`, a straight-bar Truss `truss`, GroupObject `group`, and
  anything else a `setPiece` on its `geometry/` file. Pixel strips and cameras are not exported.
- **Axes:** `transform_from_mvr` and `mvr_from_transform` convert between MVR (mm, Z up, Y upstage) and ours. Of the two
  Euler triples, the one with smaller angles is kept, so a hung fixture reads (180, 0, 0).
- **Geometry:** `parse_gltf` (cgltf) loads `.glb`/`.gltf` like OBJ. `Session::rebuild_meshes` reads both, and the
  exporter writes generated `.glb` files for walls, trusses and OBJ set pieces.
- **Session:** `importMvr(url)` merges into the open project in one undo step and renames colliding UUIDs (re-importing
  keeps existing layers). Warnings go to `resolumeWarnings`. `exportFile("mvr", url)` writes the zip.
- **CSV:** `parse_csv` detects `,`, `;` or tab. `guess_csv_mapping` matches header synonyms; there are no real
  Capture/Vectorworks presets yet. `csv_import` updates fixtures whose `fixtureId` matches and adds the rest. Session
  `csvPreview(url)` feeds the mapping dialog, and `importCsv(url, mapping, unit, zUp)` resolves type text against the
  library (file name, `Manufacturer@Name` or type name).

## 19. FBX and OBJ materials (B5)
- **`core/fbx.h`:** `load_fbx(bytes)` reads through ufbx (axes and units converted to Y-up metres) into `FbxScene`:
  nodes with a mesh somewhere below them (parent index, `Transform`, `group` index) and one `ObjGroup` per mesh
  node in node space, with its first material's base colour. `write_fbx(meshes, cameras)` is our binary FBX 7.4
  writer: world-space meshes under group nulls, emissive `_screen` quads with UVs, cameras (rotation order ZYX).
- **Render:** `export_fbx(project, library, meshes, only)` shares the gathering with `export_obj`.
- **Session:** `importFbx(url)` stores the file under `geometry/`, adds a set piece (`group` = node mesh name) per
  mesh node and a `group` object per empty node, parents kept, one undo step. `exportFile` takes `fbx` and
  `fbxSelection`. FBX meshes are keyed only per group (`geometry/x.fbx#Name`), never as a whole file.
- **OBJ materials:** `parse_mtl`, `obj_mtllibs`; `importObj` copies the MTL files named by `mtllib` into
  `geometry/`. `Mesh::colour` (alpha 0 = default grey) tints set pieces.
- **AT-4 check:** `tools/blender_check.py` opens the FBX and OBJ that `sv_session_tests` leaves in
  `/tmp/sv_session_tests/` and asserts names, metres, Z-up in Blender and the camera direction. CI runs it with
  apt's Blender; locally use a `pip install bpy` venv.


## 20. Plan and elevation drawings (B6)
- **`render/drawing_painter.h`:** `paint_drawing(painter, project, library, meshes, sheet)` paints a whole sheet in
  page millimetres from `build_scene` at rest: box hulls, walls (a line in plan), fixture symbols by type name (spot,
  wash, beam, blinder, pixel), automatic dimensions (overall stage, fixture rows, truss trims above the deck top in
  elevations), manual dimensions and the title block. It returns drawing metres → page mm. `drawing_point(kind, p)`:
  plan `(x, z)`, front `(x, -y)`, side `(-z, -y)`.
- **Stored sheets:** `project["drawings"]`, one `{uuid, kind, sheet, scale, title, hiddenLayers, dimensions}` per
  kind, created on first edit. `Session.drawing(kind)`, `setDrawing(kind, key, value)` (one undo step),
  `exportDrawing(kind, url)` (PDF via `QPdfWriter`, SVG via `QSvgGenerator`, by suffix). QtSvg is LGPL-3.0.
- **UI:** `DrawingItem` (QQuickPaintedItem) on `PlanScreen.qml`; Shift-drag adds a dimension snapped to 5 cm.
  Drawings are paper white with fixed ink colours on purpose; the Theme rule covers the app chrome, not the sheet.
- **Goldens:** `tests/golden/plan.png` and `front.png` are rendered without lettering (`sheet.text = false`) by
  `sv_session_tests`; regenerate with `SV_UPDATE_GOLDEN=1`.

## 21. NDI, Spout and Syphon (B2)
- **Kinds:** `MediaSource::open` takes `ndi`, `spout` or `syphon` with a sender name. Every receiver runs on its own
  thread and hands BGRA `QImage`s to the same latest-wins slot as video and capture.
- **NDI (`media/ndi.h`):** `third_party/ndi/` holds the MIT SDK headers only. `ndi_api()` loads the runtime at run time
  from `$NDI_RUNTIME_DIR_V6`, then the system path; it is never linked or shipped. `ndi_sources()` blocks for about a
  second, so it never sits in a QML binding (NFR-9). `NdiReceiver` asks for the highest bandwidth and BGRX/BGRA, drops
  audio, and leaves reconnect-by-name to the runtime. The `[ndi]` session test is a loopback sender, skipped without a
  runtime. NDI compresses even on loopback, so pixel checks need a tolerance.
- **Spout (`spout_receiver.cpp`, Windows only):** this is our own reader, not the Spout SDK. It reads the
  `SpoutSenderNames` and per-sender `SharedTextureInfo` shared memory, opens the legacy D3D11 shared handle under
  `<name>_SpoutAccessMutex`, and copies the texture through a staging texture. It is compile-checked with MinGW only.
- **Syphon (`syphon_receiver.mm`, macOS, `SV_WITH_SYPHON`):** it builds the client-side sources of Syphon-Framework at
  a pinned commit and subclasses `SyphonClientBase`, then locks and copies the IOSurface. It has never been compiled
  here.
- **NativeReceiver:** one file per platform defines `create` and `senders`; `native_receiver_none.cpp` covers Linux.
- **Latency (NFR-3):** `MediaStatus::latency_ms` is the smoothed time from a frame's arrival to the render thread
  taking it. The Media card shows it next to the frame rate.

## 22. Installers, updates and crash reports (B7, AT-7)
- **Version:** `project(... VERSION)` in the top `CMakeLists.txt` is the one source. It reaches the app as `SV_VERSION`,
  the MSI as `-d Version=` and the appcast as the tag. `Session.appInfo` gives QML `version`, `updates`, `crashUploads`
  and `crashFolder`. The Project screen's ABOUT section shows them with the licence notes.
- **Preferences:** `Session.preference(key, fallback)` and `setPreference` read and write `QSettings`, not the project.
  The keys are `autoUpdate`, `updateChannel` (`stable` or `test`, used at the next launch) and `crashReports`.
- **Updates (`app/updater.h`):** WinSparkle 0.9.4 on Windows and Sparkle 2.10.0 on macOS, both checking one EdDSA key.
  They are compiled in only when `SV_APPCAST_BASE` and `SV_WINSPARKLE_DIR` or `SV_SPARKLE_DIR` are set (CI downloads
  both). The feed is `<base>/appcast.xml`, or `appcast-test.xml` for the Test channel. Linux and local builds get
  stubs, and `--screenshot` never starts the updater.
- **Appcast (`tools/make_appcast.py`):** `keygen` prints a key pair. `build` writes one item with one signed enclosure
  per installer (msi/exe windows, dmg/zip macos). The `appcast_tool` ctest runs its self-test and skips (77) without
  `cryptography`. On a `v*` tag, CI's release job creates the GitHub release and pushes the feed to `gh-pages`;
  `-test` tags become prereleases on the Test feed.
- **Crash reports (`app/crash_reports.h`):** with `SV_SENTRY_DSN`, Sentry Native 0.17.4 (FetchContent) starts with
  `require_user_consent`, and the ABOUT opt-in gives or revokes consent. Linux builds use the inproc backend with no
  transport, because crashpad needs libcurl. Without a DSN, Windows writes a minidump per crash to `crashFolder`, and
  macOS and Linux leave it to the OS.
- **Windows installer (`installer/windows/SceneVisualiser.wxs`, WiX 5):** per machine, MajorUpgrade, the `.svproj`
  association, a Start-menu shortcut, and a private-profile firewall exception for the program (Art-Net, sACN, NDI).
  It has no dialog UI.
- **macOS:** `installer/macos/Info.plist.in` declares local-network use, `_ndi._tcp` Bonjour, the `.svproj` document
  type and `SUPublicEDKey`. CI builds `arm64;x86_64`, then signs inside-out with `entitlements.plist` and the hardened
  runtime, notarizes and staples, but only when the `MACOS_*` secrets exist. Otherwise the dmg is unsigned.
- **CI secrets and variables:** `vars.SV_UPDATE_PUBLIC_KEY`, `secrets.SPARKLE_ED_PRIVATE_KEY`, `secrets.SV_SENTRY_DSN`,
  `MACOS_CERT_P12` (base64), `MACOS_CERT_PASSWORD`, `MACOS_SIGN_IDENTITY`, and `MACOS_NOTARY_KEY` (base64 .p8),
  `MACOS_NOTARY_KEY_ID` and `MACOS_NOTARY_ISSUER`. Windows uses Azure Trusted Signing, which signs the exe, the DLLs
  and then the MSI, and needs `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, `AZURE_SIGNING_ENDPOINT`,
  `AZURE_SIGNING_ACCOUNT` and `AZURE_CERT_PROFILE`. Every signing step is skipped while its secrets are missing.
- **Opt-in:** Sparkle and WinSparkle both ask before the first automatic check (NFR-9). Crash uploads are off until
  the ABOUT checkbox is ticked.

## 23. Save, open and recent files
- **Unsaved changes:** New, Open, Recent, a dropped `.svproj` and closing the window all go through `guarded(action)` in
  `Main.qml`. When `dirty`, it asks Save / Don't save / Cancel. Save on an untitled project runs Save as first.
  `Session.discardChanges()` deletes the autosave, so discarded edits are not offered for recovery.
- **Recent files:** `Session.recentFiles` holds `{path, name, url}`, newest first, at most 10, and lists only files that
  still exist. It is stored in QSettings `recentFiles` on every successful open and save. The tests point QSettings at a
  temp folder.
- **Drop and Finder:** dropping onto the window opens files by extension: `svproj`, `gdtf`, `mvr`, `obj` (metres, Y up),
  `fbx` and `xml` (Resolume). macOS `QFileOpenEvent` arrives as `Session.openRequested(url)`. Ctrl+Shift+S runs Save as.
- **Missing files:** `Session.missingFiles` lists fixture types, geometry and media that the project names but cannot
  find. `projectOpened()` shows them in a dialog. `linkedMedia` lists media linked by absolute path, and `packMedia()`
  copies it into `media/` as one undo step.

## 24. AI access (MCP) and debug mode
- **Plan:** `docs/mcp-plan.md` (decisions A1–A9). `sv_mcp` (`src/mcp`, std + Asio + nlohmann, no Qt) is an MCP
  Streamable HTTP server: `POST /mcp` with plain JSON replies, no SSE, `Mcp-Session-Id`, Bearer token, a local-only
  `Origin` check and a 4 MB body limit. `Server::handle(Request)` works without sockets for tests.
- **Tools (`app/ai_access.cpp`):** `make_mcp_tools(Session&)` builds the table. Every tool hops to the UI thread
  (`on_ui`, at most 10 s). Edits go through `Session::try_apply` and `create_object`, labelled `AI: …` by the
  label prefix, so each is one undo step. File tools need `ai_files()` and debug tools `debug_mode()`; both are
  `Tool::available`. Add a tool there, and a test in `session_tests.cpp` (`tool()` helper).
- **Preferences:** `aiAccess`, `aiLan` (bind 0.0.0.0), `aiFiles`, `aiPort` (7311) and `aiToken` (32 random bytes, hex).
  `--mcp` turns access on for one run; `--screenshot` runs never listen without it. `Session.aiStatus` feeds the
  Project screen's `AiPanel.qml` and the title-bar `AI ●`.
- **Debug mode:** `--debug` (this run) or the Project screen toggle (preference `debug`). It enables `sv.*.debug`, the
  HUD (`Session.renderStats`, published by the viewport renderer every 0.5 s) and the debug tools. `main.cpp`'s
  message handler feeds `Session::append_log`, a 2000-line ring read by `get_log` and the log card.
- **Checks:** `tools/mcp_probe.py [--url --token]` (stdlib) runs handshake, screenshot, edit and undo against a running
  app; ctest `mcp_e2e` runs it under Xvfb on port 7399 with scratch settings.
