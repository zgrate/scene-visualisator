# AGENTS.md — Scene Visualiser

## 1. What this is
A native desktop previsualiser for stage lighting and LED walls, in the style of Capture. It receives DMX over Art-Net
and sACN, decodes fixtures from GDTF, and renders beams and walls on the GPU. It is C++20 with Qt 6.12 Quick and QRhi.
- **Spec:** https://artifacts.zgrate.ovh/a/KGhqn7zy8Lwo. The local source is
  `.artifacts/fragments/scene-visualiser/body.html` (product docs: FR/NFR, protocols, `.svproj`, acceptance tests,
  roadmap) and `mockups.html` (design 2, "Studio").
- **Current target:** the roadmap's **Alpha** row, with the Studio design in a **dark** palette.
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
```
- **Claude Code:** `.claude/settings.local.json` sets both environment variables, which are machine-specific and gitignored.
- **Outputs:** binaries land in `build/<preset>/bin/`.
- **`--screenshot <png>`** grabs the window after the first frame, then exits with 0 on success.
- **CI** (`.github/workflows/ci.yml`) runs ubuntu-24.04, windows-2022 and macos-14. Each runs the tests and a smoke
  screenshot, uploaded as an artifact.
- **Windows cross build:** `win-mingw` is a local cross-compile preset. It needs `QT_ROOT_DIR` set to Qt's `mingw_64`
  and `QT_HOST_PATH` set to `gcc_64`.
- **No GPU here:** this machine renders with Mesa llvmpipe. Frame-rate numbers from it are trends, never targets.

## 3. Layout & boundaries
| Target | Path | May use |
|---|---|---|
| `sv_core` | `src/core` | std, nlohmann_json, minizip-ng. **No Qt.** Model, `.svproj`, undo, exporters, OBJ |
| `sv_gdtf` | `src/gdtf` | std, pugixml, minizip-ng. **No Qt.** GDTF reader, DMX→attribute decoder |
| `sv_proto` | `src/protocol` | std, Asio, ETCLabs sACN. **No Qt.** Art-Net/sACN receivers, merger, triple buffer |
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
- **Commits:** cite spec ids in commits and test names, for example `FR-9`, `NFR-4` or `AT-6`.

## 5. Spec references
- **Requirements:** FR-1..28 are functional and NFR-1..12 non-functional. Acceptance tests are AT-1..7. All are in
  `body.html`.
- **Art-Net 4:** UDP 6454. The Port-Address is 15 bits (Net 7, Sub-Net 4, Universe 4). Supported packets are ArtPoll,
  ArtPollReply, ArtDmx and ArtSync. Out-of-order sequence numbers are dropped. The timeout is 3 s.
- **sACN E1.31:** UDP 5568, multicast `239.255.U_hi.U_lo`. It covers priority 0–200, per-address priority (start code
  0xDD), sync, and a 2.5 s timeout.
- **Merge:** each universe merges by HTP, LTP or priority. Internal generators are just another source.
- **`.svproj`:** a zip of `project.json` (with `schemaVersion`) plus `gdtf/`, `geometry/`, `media/` and `exports/`.
  Writes are atomic (temp file then rename), with 3 backups and a 2-minute autosave. Every object has a UUID. Unknown
  keys are preserved.

## 6. Design rules (Studio, dark)
- **Layout:** the 3D view is the page, with a 36 px icon rail on the left (Design, Patch, DMX monitor, Project).
  Floating cards sit on top: Objects at top-left, Inspector at top-right. They can be dragged and collapsed, and their
  positions persist in the project's `layout`.
- **Dock:** a bottom dock holds the 512-bar universe meter. It is a C++ `QQuickItem` with one geometry node, never a
  Repeater.
- **Title bar:** the project name, the tabs Design/Plan/Patch/Mapping, and the Art-Net/sACN status.
- **Colours:** use the `Theme` QML singleton only. Components contain no colour literals. Use the Fusion style with the
  palette set in `main.cpp`.

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
- **Keyboard:** Ctrl (Cmd on macOS) with Z or Shift+Z for undo and redo. Delete removes the selection, and arrows nudge
  it.

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
- **D3:** Qt under LGPL v3 with dynamic linking, confirmed by the user on 2026-10-22. Ship Qt as replaceable shared
  libraries, never statically linked. Use only modules offered under LGPL; check `$QT_ROOT_DIR/sbom/*.spdx` before
  adding one. Build tools such as qsb and moc are GPL with Qt's exception, so their output carries no obligation.
- **D4:** FetchContent for the alpha, not vcpkg. Switch to a vcpkg manifest in Beta (NDI, Spout, FBX).
- **D5:** our own GDTF reader on pugixml and minizip-ng, not libMVRgdtf (licence and Boost/Xerces). No GDTF 3D models
  in alpha.
- **D6:** built-in meshes for fixtures, trusses and walls. OBJ import produces static set pieces.
- **D7:** Qt Quick/QML, not Widgets.

## 9. Deferred (Beta/Release, do not build in alpha)
- **Media and formats:** Spout, Syphon and NDI in or out, live composition, FBX, MVR.
- **Views:** plan/elevation and the mapping view.
- **Distribution:** signed installers and updaters.
- **Rendering:** shadows and emissive wall lighting.
- **Other:** the GDTF Share browser, DMX recorder, render export and headless render node.
