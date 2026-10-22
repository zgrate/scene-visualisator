# Scene Visualiser — Alpha implementation plan (Design 2 "Studio", dark mode)

## Context

The spec is the proof-of-concept artifact at https://artifacts.zgrate.ovh/a/KGhqn7zy8Lwo. Its source is already in this
folder: `.artifacts/fragments/scene-visualiser/body.html` (lines 19–420 are the product docs: platform assumptions,
architecture, data model, DMX/media pipelines, rendering, screens, interchange formats, stack, FR-1..28, NFR-1..12,
protocol spec, `.svproj` file spec, acceptance tests, roadmap) and `mockups.html` / `mockups.css` (six design directions;
`.m2` is "Studio"). The docs state the product is a native C++20 / Qt 6 desktop app and that **no POC code is carried
over**; the POC's `app.js` is only a behavioural reference (fixture decode, generators, Resolume/MA3/CSV shapes).

**Alpha scope (roadmap row "Alpha")**: native app, Art-Net + sACN in-process, GDTF import, OBJ import/export, GPU renderer
with volumetric beams, patch sheet, project files on disk. UI: the Design screen in **Studio** posture (3D view is the page,
slim icon rail, floating Objects/Inspector cards, DMX bar-meter dock, blue accent) in a **dark** palette.

**Dev machine facts (checked)**: Debian 12, g++ 12.2, CMake 3.25 (below the spec's 3.28 floor), no ninja/vcpkg/clang-format,
**no GPU** (VMware SVGA, Mesa llvmpipe software OpenGL, no display, lavapipe not installed). Qt 6.5.3 at `~/Qt/6.5.3`
(`gcc_64` Linux, `mingw_64` Windows) without `qtshadertools` (`qsb` missing); QRhi is still a private API in 6.5.
MinGW cross compiler at `~/.local/mingw`. `xvfb-run`, ImageMagick `import`, docker, node, python3 present. The user's
other Qt project `/home/debian/gradient-generator` has reusable pieces: `cmake/mingw-w64.cmake`, `tools/screenshot.sh`,
`tools/a11y-launch.sh`, `tools/package_win.sh`, the gui-user MCP (`.mcp.json`), and the Qt plugins listed below.

## Decisions (assumptions; confirm or override at review)

| # | Decision | Why |
|---|----------|-----|
| D1 | **Linux is the daily dev build** (QRhi OpenGL on llvmpipe under `xvfb-run`, `LIBGL_ALWAYS_SOFTWARE=1 QSG_RHI_BACKEND=opengl`); Windows (MSVC, D3D12) and macOS (Metal) build in GitHub Actions from day one, with render smoke tests on the Windows WARP software adapter and the macOS runner GPU. | The spec says Win/mac only, but nothing in alpha is platform-bound (Spout/Syphon are Beta). Verifying on Linux keeps the loop fast; CI proves the other two. Frame-rate numbers (NFR-2) cannot be produced on this GPU-less box at all; they need a real laptop before M5. |
| D0 | Toolchain upgrades via pip in user space: `pip3 install --user cmake ninja aqtinstall` (CMake ≥ 3.28, Ninja). No sudo needed. | Spec requires CMake 3.28+; the system 3.25 stays for other projects. |
| D2 | **Qt 6.12.0 LTS** (confirmed present on download.qt.io as `qt6_6120`) installed to `~/Qt/6.12.0` with `aqtinstall` (`pip install aqtinstall`; `aqt install-qt linux desktop 6.12.0 -m qtshadertools qtsvg`), and the same version in CI via `jurplel/install-qt-action`. | Matches the spec's stack row. 6.5.3 lacks public QRhi, `QQuickRhiItem` (6.7+) and `qsb`. Keep 6.5.3 untouched for the other project. Fallback if 6.12.0 has an early-release bug: 6.8.3 LTS. |
| D3 | **Qt LGPL v3, dynamic linking**, no commercial licence for alpha. | Spec requires the decision before first commit; LGPL is reversible later. |
| D4 | **CMake FetchContent** for third-party code in alpha, not vcpkg. | vcpkg is not installed and alpha deps are all small CMake/header-only projects. Switch to a vcpkg manifest in Beta when NDI/Spout/FBX arrive. Deviation from the spec stack table, deliberate. |
| D5 | **GDTF: own reader** on pugixml + minizip-ng for the alpha subset (FixtureType, DMXModes, DMXChannels, ChannelFunctions with PhysicalFrom/To, Wheels with slot colours), following the public spec at github.com/mvrdevelopment/spec. GDTF 3D models are **not** rendered in alpha (simple body). `libMVRgdtf` is dropped for alpha. | Checked: libMVRgdtf has no releases, requires Boost + Xerces-C, and ships a custom "MVR SDK License" (Apache-style with extra GDTF-Group terms) that the spec says needs legal review. The spec names this exact fallback. A GDTF reader for this subset is a few hundred lines; MVR (Beta) can revisit libMVRgdtf after legal review. |
| D6 | Fixture bodies, trusses, walls use built-in meshes; OBJ import creates static set pieces (FR-3). | Alpha scope. |
| D7 | UI is Qt Quick/QML (per spec), **not** Widgets, unlike gradient-generator. | Floating cards over a GPU viewport, rail and dock are natural in Quick; the spec says Quick. |

## Repository layout and targets

```
scene-visualisator/
  CMakeLists.txt            C++20, options SV_BUILD_TESTS, SV_BUILD_APP
  CMakePresets.json         linux-debug, linux-release, win-msvc, mac, win-mingw (local cross)
  cmake/deps.cmake          FetchContent: nlohmann_json, pugixml, Catch2 v3, spdlog, tinyobjloader, asio, minizip-ng, ETCLabs/sACN
  cmake/mingw-w64.cmake     copied from gradient-generator
  src/core/     lib sv_core   (no Qt)  Project, Venue, SceneObject{Fixture,Wall,Truss,SetPiece,Camera}, Patch, Units, UndoStack,
                                       ProjectFile (.svproj = zip; project.json + gdtf/ geometry/ media/ exports/), ObjImport, ObjExport,
                                       exporters: PatchCsv, Ma3PatchXml, ResolumeAdvancedOutputXml (+ import)
  src/gdtf/     lib sv_gdtf   (no Qt)  GdtfLoader → FixtureType{modes, channels, channelFunctions, wheels, physical ranges}; AttributeDecoder (DMX → physical attributes)
  src/protocol/ lib sv_proto  (no Qt)  ArtNetReceiver (own, ArtPoll/ArtPollReply/ArtDmx/ArtSync), SacnReceiver (ETCLabs), UniverseMerger (HTP/LTP/priority),
                                       FrameQueue (SPSC per universe), InternalSources (generators, fader page, paste-frame)
  src/render/   lib sv_render (Qt Gui) QRhi renderer: scene pass, beam pass, wall pass, picking; shaders/*.vert|.frag → qsb at build (qt_add_shaders)
  src/app/      exe SceneVisualiser    main.cpp, ViewportItem (QQuickRhiItem), models exposed to QML, qml/ (Studio dark)
  tests/        Catch2 per lib; approval files tests/approved/*.{xml,csv,obj}; fixtures/*.gdtf *.obj; headless render smoke
  tools/        artnet_send.py, sacn_send.py (test senders), screenshot.sh, a11y-launch.sh (adapted), package_win.sh (later)
  .github/workflows/ci.yml   ubuntu-24.04 (tests + Xvfb smoke), windows-2022 (MSVC), macos-14; upload app bundles
  AGENTS.md, CLAUDE.md, .claude/settings.json, .mcp.json, .clang-format, .editorconfig, .gitignore
```

Threading (alpha): UI thread (Qt, QML, document edits), render thread (Qt Quick scene graph, `QSG_RENDER_LOOP=threaded`
forced so Linux matches Windows/macOS; `ViewportItem : QQuickRhiItem` syncs document + camera in `synchronize()` and
decodes DMX in `render()`), one network thread (`std::thread` running an Asio `io_context`: Art-Net sockets, sACN
callbacks `post`ed in, merger, timeouts, internal 44 Hz sources). Handoff per universe is a wait-free `TripleBuffer<UniverseFrame>`
(three slots, one atomic index word with a dirty bit; producer swaps back↔middle, consumer swaps middle↔front): latest frame
wins, no allocation. The 20 Hz UI meter reads a mutex-guarded copy instead, which is fine for latency. No DMX work on the
UI thread (spec "DMX to attributes").

## Milestones (build order)

Each milestone ends green on Linux (`ctest`) and in CI on all three OSes.

### M0 — Bootstrap (toolchain, skeleton, agent files)
- Toolchain per D0/D2; `git init`; CMake skeleton with the five targets above, FetchContent deps, Catch2 wired, presets (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`), CI workflow building all three OSes and running `ctest` (Linux under `xvfb-run`).
- First window: `ApplicationWindow` with a `QQuickRhiItem` clearing to the viewport colour, plus `--screenshot out.png --exit` (via `QQuickWindow::grabWindow()`) so agents and CI can see the UI without ImageMagick.
- Write `AGENTS.md`, `CLAUDE.md` (`@AGENTS.md`), `.claude/settings.json`, `.mcp.json`, `.clang-format`, `.editorconfig` (contents below).
- Record D1–D7 in AGENTS.md "Decisions" so agents do not re-litigate them.
- Verify: `cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug` passes with one trivial test per lib; CI green.

### M1 — Core model and project file (FR-1, FR-2, FR-5 numeric part, FR-7, FR-26)
- Entities and fields exactly as the docs "Data model" table; metres, degrees, Y-up, Z toward audience; UUID per object; unknown JSON keys preserved on round trip.
- `ProjectFile`: `.svproj` zip read/write, `schemaVersion`, atomic write (temp + rename), three rolling backups, autosave timer hook (2 min), recovery file.
- Undo/redo: every edit goes through one `Document::apply(json_patch)`; the undo stack stores the inverse `nlohmann::json::diff` patch. No per-command classes.
- Verify: Catch2 round-trip tests (load→save→load byte-identical for known keys, unknown keys preserved); kill-during-write test leaves a valid previous file.

### M2 — DMX in and internal sources (FR-9, FR-11 minus recorder, FR-12 data side)
- Own Art-Net 4 receiver per docs "Protocol specification" (Port-Address 15-bit, sequence drop, ArtSync hold, ArtPollReply, 3 s timeout). ETCLabs sACN receiver with priority, per-address priority 0xDD, sync, 2.5 s timeout.
- `UniverseMerger` HTP/LTP/priority per universe; `FrameQueue`; stats (rate, loss, winning source).
- Internal sources: generators (chase, rainbow, sweep, full), fader page, paste-a-frame. Generators write into an "internal" source that merges like any other.
- Verify: unit tests with hand-built packets; `tools/artnet_send.py` and `tools/sacn_send.py` (pure python, stdlib sockets) drive the receiver on loopback at 44 Hz for 30 s with zero loss reported; sACN multicast join on `lo` tested.

### M3 — GDTF import and attribute decode (FR-4 file part, FR-10)
- Loader per D5; bundled generic GDTF files replace the POC's five generic types (make five small `.gdtf` fixtures ourselves in `tests/fixtures`, they double as the "unpatched placeholder" types).
- `AttributeDecoder`: 8/16-bit, channel-function ranges, PhysicalFrom/To interpolation → Dimmer, Pan, Tilt, Zoom, Focus, Iris, Frost, ColorRGB, ColorAdd_W, Color wheel slot, Gobo slot (index only), Shutter/Strobe.
- Verify: decode tests against known fixture files (e.g. a public GDTF Share spot); POC's self-test cases ported as Catch2 cases (centre pan/tilt at DMX 128, wheel slot ranges).

### M4 — Renderer (FR-17 core, FR-7 wall look, FR-19 camera part)
- `sv_render` on QRhi: floor grid, room box, stage deck, truss meshes, instanced fixture bodies built from the GDTF `Model` primitives (Cube/Cylinder/Base/Yoke/Head sized by L/W/H) with yoke orientation from pan/tilt, LED walls as emissive quads with pixel-pitch/module-seam shader fed by a composition texture (test patterns, mapping grid, still image; live sources are Beta), additive ray-marched beam cones (beam/field angle, zoom, iris, frost, colour) through adjustable haze. Picking by ID buffer.
- Camera: perspective orbit/pan/zoom, Top/Front/Side presets, bookmarks stored in project.
- Shaders in Vulkan-GLSL compiled with `qsb` via `qt_add_shaders` to SPIR-V/HLSL/MSL.
- Use QRhi's portability helpers from day one (`clipSpaceCorrMatrix`, `ubufAligned`, `isYUpInFramebuffer`); stay on core features (no geometry shaders, no bindless).
- Verify: headless smoke test renders the demo rig with a pasted DMX frame to PNG and compares against a golden image with tolerance; runs in CI on Linux (llvmpipe), Windows (WARP) and macOS (Metal) and uploads the PNGs. `--bench scene.svproj` prints ms/frame; `tools/make_stress_scene.py` builds the 1000-fixture/20-wall scene. Numbers are a regression trend only until a real GPU laptop is available.

### M5 — Design screen in Studio dark (FR-5 drag part, FR-27 partial)
- QML layout: 36 px icon rail (Design, Patch, DMX monitor, Project), full-bleed `ViewportItem`, floating cards (Objects top-left, Inspector top-right; draggable, collapsible, hidden on narrow widths), bottom dock with 512-bar universe meter + universe selector + source/rate text, title bar with project name, screen tabs, status (Art-Net ● rate · sACN ● rate).
- Inspector: common (name, position, rotation, parent), fixture (type+mode picker, fixture ID, universe.address with footprint and overflow warning), wall (W, H, pitch, module, nits, gamma, slice rect, derived resolution), truss (type, length). Add fixture / wall / truss, duplicate, delete, multi-select, drag-move in viewport with grid snap. Undo/redo menu + shortcuts (Ctrl/Cmd).
- Theme singleton `Theme.qml` (`pragma Singleton`) with the dark Studio palette (below); Fusion style with the palette set in `main.cpp` so native controls inherit it. All colours via the singleton; no literals in components.
- DMX dock meter is a C++ `UniverseMeter : QQuickItem` drawing 512 bars in one `QSGGeometryNode` (a QML `Repeater` of 512 rectangles is too slow); it is reused in occupancy mode on the patch sheet.
- Cards are `Pane`s with a `DragHandler` on the header, collapsible to the header; positions persist in `layout` of project.json. The viewport stays alive under the Patch screen so tab switches are instant.
- Verify: `tools/screenshot.sh` under Xvfb, compare visually with `.playwright-mcp/m2.png` layout; gui-user MCP clicks through add/select/edit; `/qt-qml-review`.

### M6 — Patch sheet, DMX monitor, exports, alpha hardening (FR-8, FR-12, FR-23 export part, FR-24, FR-25 OBJ, FR-3)
- Patch sheet screen: `TableView` over `PatchTableModel` (ID, name, type, mode, universe, address, position), sort/filter, bulk re-address, conflict flag, universe occupancy map.
- DMX monitor screen: universe list, 512-channel grid, selected fixture highlight, winning source, rate, loss.
- Exports (via `sv_core`): patch CSV, grandMA3 patch XML, Resolume Advanced Output XML export+import (walls matched by name), OBJ import (set piece, unit/up-axis dialog, optional split per top-level group) and OBJ export (one group per object, LED walls as named quads). Default folders per docs "Interchange formats".
- Hardening: autosave + recovery on launch (AT-6), spdlog file log, crash-safe writes, CI release artefacts (unsigned zip/dmg; signing + installers are Beta).
- Verify: approval tests byte-compare exporters with `tests/approved/`; Resolume XML round trip (export→import→same walls); OBJ export opens in Blender (manual) and re-imports (test); AT-6 kill test scripted on Linux.

## Studio dark palette (derived from `.m2` + `theme.css[data-theme=dark]`)

| Token | Value | Source |
|-------|-------|--------|
| bg | `#121418` | theme.css dark bg |
| panel / card | `#1b1e24` | theme.css dark surface |
| line | `#2c3039` | theme.css dark border |
| fg | `#e8e6e1` | theme.css dark fg |
| muted | `#a09d95` | theme.css dark muted |
| viewport | `#0b0c10` | theme.css viewport-bg (Studio keeps the viewport dark already) |
| accent | `#4c8dff` | `.m2` `#0a66d6` lifted for contrast on dark panels (≈ 6.1:1 on `#1b1e24`) |
| accent-on | `#ffffff` | `.m2` |
| accent-soft (selection row) | `#1d2f4d` | dark analogue of `.m2` `#e2edfb` |
| grid / deck / truss | `#262a33` / `#23262e` / `#a8adb8` | `.m2` as is |
| status ok / warn / err | `#3ddc97` / `#ffd166` / `#ff6b6b` | colour-blind-safe trio (NFR-10) |

Card: 1 px `line` border, radius 8, shadow `0 4px 12px rgba(0,0,0,.45)`. Mono font for numbers (`ui-monospace`), system
sans for labels, 8 px uppercase tracked section headers as in the mock-up. Selection colour is the accent (Studio), not the
POC's amber.

## AGENTS.md (written in M0; CLAUDE.md contains only `@AGENTS.md`)

Sections, each a few lines, no prose essays:
1. **What this is** — one paragraph; link to the spec artifact and to the docs HTML path; "alpha scope = roadmap Alpha row; Studio dark design".
2. **Build & test** — presets, `cmake --preset linux-debug`, `ctest`, Qt path env `QT_ROOT`, Xvfb screenshot command, how CI differs.
3. **Layout & boundaries** — the five targets; `sv_core/gdtf/proto` must not include Qt; renderer reads attributes never raw DMX; no DMX work on UI thread; UI edits only through `UndoStack` commands.
4. **Conventions** — C++20, `.clang-format` (LLVM base, 4 spaces, 120 cols), `snake_case` files / `PascalCase` types, metres/degrees/Y-up internally, units convert at file boundary, `shortcut:` comment form for known limits, one Catch2 test per new branch/parser.
5. **Spec references** — FR/NFR ids to cite in commits and tests; the protocol constants (ports 6454/5568, timeouts 3 s / 2.5 s, multicast 239.255.U_hi.U_lo).
6. **Design rules** — Theme singleton only, Studio dark tokens table, card/rail/dock geometry, keyboard conventions.
7. **Agent tooling** — which skills/plugins to use for what (below), gui-user launch via `tools/a11y-launch.sh`, never the raw binary.
8. **Deferred (Beta/Release)** — Spout/Syphon/NDI, FBX, MVR, plan/elevation, mapping view, installers, shadows, GDTF Share, recorder, render export, headless node.

## Plugins, skills and MCP to enable for this repo

Already installed, enable in `.claude/settings.json` `enabledPlugins`:
- `qt-development-skills@claude-plugins-official` (installed, project-scoped to gradient-generator today): `/qt-cmake-project`, `/qt-qml`, `/qt-qml-review`, `/qt-cpp-review`, `/qt-ui-design`, `/qt-qml-test`, bundled Qt Documentation MCP for live API lookup.
- `qt-suite@l3digitalnet-plugins` (installed): agents `qt-app-reviewer`, `qt-ux-advisor`, `qt-debugger`, references on threading/QML/architecture. Its Qt Pilot MCP drives Python apps only — note in AGENTS.md as in gradient-generator.
- `ponytail@ponytail`, `frontend-design@claude-code-plugins` (user scope, already on).

Install from `claude-plugins-official`:
- `clangd-lsp` — C/C++ code intelligence (also install `clangd` + emit `compile_commands.json` via `CMAKE_EXPORT_COMPILE_COMMANDS=ON` in presets).
- `code-review` — multi-agent PR review before merges.
- `commit-commands` — commit/push/PR flow.

MCP servers in project `.mcp.json`:
- `gui-user` (user's local X11/AT-SPI driver, same entry as gradient-generator) for driving the real app under Xvfb.
- `context7` is already a user-level MCP; AGENTS.md tells agents to use it for Qt/QRhi/Catch2/Asio docs.

Project hooks in `.claude/settings.json` (via `update-config` skill at M0): PostToolUse on Edit/Write of `*.cpp|*.h` → `clang-format -i`; Stop → `cmake --build --preset linux-debug && ctest --preset linux-debug --output-on-failure` (fail loudly). Permissions allowlist: `cmake`, `ctest`, `xvfb-run`, `tools/*.sh`, `python3 tools/*.py`, `git`.

Not used: web-only plugins (playwright, superdesign), Cloudflare skills.

## Verification (end-to-end, alpha exit)

1. `ctest` green on Linux, Windows, macOS in CI; approval files unchanged.
2. `tools/artnet_send.py --universes 8 --fps 44 --pattern chase` for 30 min against the app → monitor shows 44 Hz, 0 loss, fixtures follow (AT-2 on loopback).
3. Load a GDTF Share spot, patch it, drive pan/tilt/dimmer/colour via sACN from the test sender; beam moves and colours correctly in the viewport.
4. Build the demo rig (2 trusses, 19 fixtures, 3 walls) from the UI; save `.svproj`; kill the process; relaunch offers recovery (AT-6).
5. Export CSV, MA3 XML, Resolume XML, OBJ; re-import Resolume XML and OBJ; open OBJ in Blender.
6. Screenshot under Xvfb matches the Studio layout in `.playwright-mcp/m2.png` with the dark palette; `/qt-qml-review` and `/qt-cpp-review` clean.

## Deliberately deferred

Spout/Syphon/NDI and the live composition (Beta), FBX (Beta), MVR (Beta), plan/elevation and mapping view (Beta), signed
installers and updaters (Beta), shadows and emissive-wall lighting, GDTF Share browser, DMX recorder, render export,
Spout/Syphon/NDI output, headless render node (Release). Performance target NFR-2 is measured and logged in alpha, not gated.

## Risks (ranked)

1. **QRhi on three backends**: shader or feature divergence only visible in CI → render smoke test on every OS from M4; keep to QRhi core features (no geometry shaders, no bindless).
2. **GDTF reader completeness** (own parser, D5): real-world GDTF files use features outside the alpha subset (ModeMaster, relations, sub-fixtures) → unknown elements are ignored with a logged warning, tested against three GDTF Share files of different manufacturers.
3. **Qt Quick + custom RHI item performance** (1000 fixtures, 20 walls at 60 fps) → instanced bodies and beams from M4, measure early.
4. **sACN library deps (EtcPal, own threads)** on MSVC/clang → FetchContent pinned tag, callbacks only `post` into Asio; fallback: own ~300-line E1.31 receiver on the Art-Net socket code (spec names both).
5. **Qt licence**: Qt's commercial terms penalise switching from open source later, so D3 (LGPL dynamic) must be confirmed before the first commit.
6. **Local Linux ≠ shipped platforms**: no GPU here, Spout/Syphon cannot be exercised at all; NFR-2 measured on a borrowed RTX 3060 / M1 laptop before M5.
