# Scene Visualiser — Beta implementation plan

## Context

Alpha (M0–M6, commits 2b8f575..34be32d) is done. It covers native Art-Net and sACN, GDTF, the QRhi renderer, the Studio
dark Design, Patch, DMX and Project screens, `.svproj` with autosave, and the CSV, MA3, Resolume and OBJ interchange.
Beta is the spec roadmap row (`body.html` line 409):

> Spout, Syphon and NDI input, FBX import and export, MVR round trip, plan and elevation drawings, media mapping view,
> signed installers with in-app updates

The user also asked for **Resolume integration** as a first-class feature, not only the alpha's Advanced Output file
exporter. The workflow it must support is this: a designer builds walls in the visualiser, sends the mapping to Arena,
Arena sends its composition back over Spout, Syphon or NDI, and the walls show it. That is AT-3, so the Resolume work
and the media work are one milestone chain.

**The real show file.** The user supplied a real show file:
- **Files:** an Arena 7.17.3 Advanced Output file plus Arena's slice SVG, saved as
  `tests/fixtures/resolume/arena-7.17-futro-ajki26.{xml,-input.svg}`.
- **What it holds:** a 2500×1080 composition with two kinds of output, and the visualiser must preview both.
  - **Video screens:** two `<Screen>`s, each an `OutputDeviceNDI` feeding an LED processor ("NDI NOVASTAR" 1920×1080,
    "NDI LINSN TECHWALL ROMBY" 1024×768).
    - The two screens hold four `<Slice>`s: a main wall, a narrow strip, and two diamonds rotated 45°.
    - Each slice has a 4-point `InputRect` in composition pixels with an `orientation` in radians, an `OutputRect` in
      processor pixels, and a `Warper` (a 4×4 Bezier grid and a homography).
    - **One screen is an output and holds several walls.** The alpha importer assumed one wall per screen and read only
      the first slice's bounding box, so it gets this file wrong.
  - **LED pixel strips:** eight `<DmxScreen>`s (WLED portals, gates and walls), each a Lumiverse (`LumiverseId` 0–8).
    - The eight screens hold 41 `<DmxSlice>`s: thin rotated quads, each with an inline fixture of N×1 RGB pixels
      (60–450 px, 2.5 gamma, Distribution 170).
    - Each strip has a `Start Channel` (1…1843) that runs on past 512, across universes.
- **Where the Art-Net goes is not in the file.** The Lumiverse target is `TT_DISABLED` with subnet and universe 0, so
  the per-Lumiverse universe and IP live in Arena's DMX output preferences.

Arena already sends these strips to the WLED controllers over Art-Net, and the alpha's receiver can take that same
Art-Net. So the strips can be previewed two ways:
- **Exact:** live DMX from Arena.
- **Offline:** sampled from the composition, with no Arena running.

**Requirements covered:**
- **Media:** FR-13 (live input), FR-14 (fallback media), FR-15 (slices and the mapping view), FR-16 (pixel fixtures
  from a slice)
- **Interchange:** FR-3 (FBX import), FR-22 and FR-23 (MVR and CSV import), FR-24 (Resolume in depth), FR-25 (FBX
  export)
- **Drawings:** FR-19 (plan and elevation)
- **Non-functional:** NFR-3 (media latency), NFR-9 (no listening ports)
- **Acceptance tests:** AT-1 (MA3 MVR round trip), AT-3 (Arena), AT-4 (FBX/OBJ with Blender), AT-7 (installers and
  updates)

**Still true from alpha:**
- This machine has no GPU and no Windows or macOS hardware.
- Spout and Syphon can only be compiled in CI and tested on the user's own machines.
- NDI is the exception: its SDK runs on Linux, so the whole NDI path, the media layer and the mapping view can be built
  and tested here.

## Decisions (confirm or override at review)

| # | Decision | Recommendation and why |
|---|----------|------------------------|
| B1 | **Windows graphics backend** | **Force D3D11** on Windows with `QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11)`. Spout shares DX11 textures, so on D3D11 a sender's texture opens straight into our device and costs one GPU copy. On D3D12 it needs D3D11On12 interop and fences. NFR-3 asks for under one frame, and D3D11 is the simple path. Qt's default on Windows is already D3D11, so we pin it rather than switch. |
| B2 | **FBX export** | **Write our own binary FBX 7.4 writer** (about 600 lines: nodes, mesh geometry, materials, cameras, hierarchy). Each export is checked by re-reading it with ufbx in a test and by Blender in CI. The spec's choice, the Autodesk FBX SDK, has a licence that needs legal review, cannot be redistributed, and is closed source next to our LGPL app. Assimp's FBX exporter is weak on hierarchy and materials. ASCII FBX is not an option because Blender refuses it. Fallback if Blender or C4D reject our files: the Autodesk SDK as a separately downloaded plug-in, after legal review. |
| B3 | **NDI** | Use NDI SDK 6.x headers at build time, **load the runtime dynamically** (`NDIlib_v6_load`, the documented way), and **leave NDI discovery off by default** (NFR-9). Without the runtime, the source list shows "NDI runtime not installed" with the download link. We ship the runtime in installers only if the SDK terms allow it after review; otherwise the user installs NDI Tools. NDI's brand rules (the "NDI®" name, the link in About) apply. |
| B4 | **Video files and screen/window capture (FR-14)** | **QtMultimedia**: `QMediaPlayer` into `QVideoSink`, plus `QScreenCapture` and `QWindowCapture`. These are LGPL, and their FFmpeg backend is LGPL as Qt builds it. Check `$QT_ROOT_DIR/sbom/*.spdx` per D3 before adding the module. Frames reach the render thread through the same latest-wins handoff as NDI. |
| B5 | **MVR** | **Our own MVR 1.6 reader and writer** on pugixml and miniz, extending D5. GDTF files are embedded and read with our reader. **glTF/GLB geometry is read with cgltf** (MIT, single header). 3DS geometry is shown as a bounding-box placeholder and its file is kept for the round trip. Unknown XML on each object is stored verbatim, so a round trip loses nothing. MVR-xchange (live TCP sharing) is **out of scope** because it needs listening ports (NFR-9). |
| B6 | **How deep Resolume integration goes** | Three layers, all outbound, none listening: (1) **Advanced Output preset round trip**, rewritten to match real Arena 7 files and written into Arena's presets folder; (2) **auto-discovery of Arena's composition stream**: pick the Spout or Syphon sender, or NDI source, named like Arena and follow its size; (3) an **optional Arena REST client** (Arena's webserver, default port 8080, enabled by the user in Arena's preferences). It reads the composition size and layer and clip names and can trigger a clip or column from the mapping view. The REST API cannot load Advanced Output presets, so step (1) still needs one click in Arena. |
| B7 | **Code signing** | Windows: **Azure Trusted Signing** (cheaper than an EV certificate, and it builds SmartScreen reputation) through `azure/trusted-signing-action`. macOS: a **Developer ID Application** certificate plus `notarytool` with an App Store Connect API key. **The user must supply these** as GitHub secrets. Until then the pipeline builds unsigned and every signing step is skipped. |
| B8 | **Updates** | **Sparkle 2** on macOS and **WinSparkle** on Windows, both reading one EdDSA-signed appcast published to GitHub Pages from release CI. Checks are off until the user opts in on first launch (NFR-9 privacy). There is no Linux updater because Linux is dev only. |
| B9 | **Crash reporting** | **Sentry Native** with the crashpad backend, **opt-in**, and the DSN from a build secret. Without a DSN it is compiled out. A local minidump folder is always written, so a user can attach dumps by hand. |
| B10 | **Dependency manager** (D4 said "switch to vcpkg in Beta") | **Stay on FetchContent; this re-opens D4, so the user decides.** None of the Beta dependencies needs vcpkg. ufbx, cgltf, Spout2 and Sentry Native all work with FetchContent. The NDI SDK, Sparkle and WinSparkle are vendor downloads into a CI cache. vcpkg would add a second system for no gain. |
| B12 | **LED pixel strips** | A **built-in `pixelStrip` object**, not a GDTF type. The sample file has 41 strips of 13 different lengths, which would need a generated GDTF per length. A strip is fully described by its pixel count, colour order and patch, which is all WLED or Resolume knows about it. The decoder maps channels to pixels with Resolume's universe rule (**Distribution**: N pixels per universe, then the next universe starts at channel 1) and is unit-tested. The renderer draws strips as wall-like emissive cells, so they cost one extra instance list, not a new pass. |
| B11 | **Spout, Syphon and NDI output of the 3D view** | It is in the spec's **Release** row, not Beta. It fits the Resolume story (the visualiser as a source in Arena), so it is listed here as an **optional milestone, B8**, to pull in only on request. |

## Model changes (schemaVersion 2)

`project.json` gains only the following. A `migrate_1_to_2()` function in `core/project_file.cpp` upgrades version 1
files on load. Unknown keys stay preserved.

- **`composition`:** it already holds `width`, `height`, `frameRate` and `source`. `source` becomes an object,
  `{kind: testPattern|grid|still|video|capture|spout|syphon|ndi, name, file, fit}`. `media/` in the package holds stills
  and video files, or links to them when larger than 200 MB.
- **`layers`:** a top-level array of `{uuid, name, visible, locked}`, and every object gains `layer` (a uuid). This is
  needed for MVR, which is layer-based, and for drawings.
- **Fixture `pixelSource`:** `{kind: dmx|slice, slice: {x,y,w,h}}` (FR-16). It is only meaningful for types with pixel
  `cells`.
- **Object `extras.mvr`:** the verbatim XML of the object's unknown MVR children, plus its MVR UUID and its geometry file
  names.
- **`resolume`:** `{presetName, restHost, restPort, restEnabled}`, in `layout`-like settings outside undo, because it is
  machine-specific.
- **`outputs` (Resolume screens):** `{uuid, name, kind: ndi|spout|display|lumiverse, deviceName, width, height,
  lumiverseId, universe}`. A wall or pixel strip has `output` (a uuid). Arena's screens map to outputs; its slices and
  DMX slices map to our objects.
- **Wall `slice`:** keeps `{x,y,w,h}` as the unrotated input rect and gains `angle` (degrees about the rect centre) and
  `outRect` (in the output's pixels). The Warper XML is stored verbatim in `extras.resolume` and written back unchanged.
  Bezier warps are not previewed (`shortcut:`).
- **New object type `pixelStrip`:**
  - Fields: `{pixels, rows, colorFormat: rgb|grb|rgbw|…, gamma, length, patch: {universe, address}, distribution,
    pixelSource: dmx|slice, slice}`.
  - 3D look: a line or grid of emissive cells.
  - Decision B12 explains why this is not a GDTF fixture.
- **`drawings`:** a list of `{uuid, kind: plan|front|side, sheet: A3|A4|…, scale, title, viewBox}`, so sheets survive
  reopening.

## Architecture additions

```
src/media/   lib sv_media (Qt Gui + QRhi, no Quick)   CompositionSource interface and its implementations
  composition_source.h   struct CompositionFrame { QRhiTexture* or QImage; QSize; double fps; quint64 serial; }
                         class CompositionSource { virtual void open(QRhi*); virtual CompositionFrame acquire(); ... }
  pattern_source.cpp     test pattern, mapping grid with slice outlines and names, still image (moved from test_pattern)
  qimage_source.cpp      QtMultimedia video, screen and window capture -> latest-wins QImage -> texture upload
  ndi_source.cpp         NDI receive on a media thread, BGRA, double-buffered upload (all OSes, Linux too)
  spout_source.cpp       Windows: Spout2 receiver -> OpenSharedResource -> CopyResource -> QRhiTexture::createFrom
  syphon_source.mm       macOS: SyphonMetalClient -> newFrameImage (MTLTexture) -> QRhiTexture::createFrom, no copy
  source_registry.cpp    lists available senders per kind (Spout sender list, Syphon directory, NDI finder)
src/core/   + mvr.cpp, fbx_writer.cpp, csv_import.cpp, resolume.cpp (moved out of interchange.cpp and extended), drawing.cpp
src/render/ + drawing_painter.cpp (QPainter: plan and elevation, also used for PDF)
src/app/    + mapping_item.cpp (QQuickRhiItem: composition plus slices), drawing_item.cpp (QQuickPaintedItem),
              resolume_client.cpp (QNetworkAccessManager, Arena REST), updater.cpp, qml/MappingScreen.qml, qml/PlanScreen.qml
installer/  windows/*.wxs (WiX v5), macos/entitlements.plist, Info.plist additions, appcast template
```

**How the threads change:** the render thread owns the `CompositionSource` and calls `acquire()` once per frame.
- **Spout and Syphon:** `acquire()` runs on the render thread; it is GPU work only.
- **NDI and QtMultimedia:** these deliver on their own threads into a mutex-guarded latest-frame slot. The render thread
  uploads only when `serial` has changed.
- **Shared texture:** `SceneRenderer::set_composition` changes from taking a `QImage` to taking a `QRhiTexture*`, which
  the source owns. The mapping view's `MappingItem` samples the same texture.
- **Two windows:** if the mapping view opens in its own window, which has its own QRhi, it falls back to a CPU copy.
  This is marked `shortcut:`.
- **Session:** it holds only the source description and a status: name, resolution, fps and "no signal".

## Milestones

Each milestone ends green on Linux (`ctest`) and in CI on all three OSes, and is committed with spec ids and **no
co-author trailer**.

### B0 — Beta bootstrap
- Version 0.2.0 and schemaVersion 2 with its migration and a test that loads an alpha `.svproj`.
- Add `sv_media`. FetchContent: ufbx, cgltf and Spout2 (Windows only).
- A CI step downloads and caches the NDI SDK on all three OSes and the Syphon framework on macOS. Check the QtMultimedia
  SBOM, then add the module.
- Pin D3D11 on Windows (B1).
- Update AGENTS.md: the Beta target, B1–B11, the `sv_media` row in section 3, and Beta moved out of "Deferred".
- Title-bar tabs Plan and Mapping are switched on but show empty screens.

### B1 — Media layer and fallback media (FR-14, FR-13 UI)
- Add the `CompositionSource` interface, and move test pattern, grid and still into `pattern_source`. The grid draws each
  wall's slice outline and name, so mapping mistakes show in 3D.
- Add `qimage_source`: video files (loop, play and pause, rate), screen capture and window capture.
- Add a **Media card** in the dock area. It has a source-kind picker and a source list filled by `source_registry`. It
  shows "1920×1080 · 59.9 fps", or "No signal" in the warn colour.
- Verify: Catch2 tests for the source registry and grid geometry. A golden PNG of the grid with three slices. A
  screenshot with a video file playing on the walls.

### B2 — NDI, Spout, Syphon (FR-13, NFR-3)
- **NDI (built and tested here):**
  - Finder with discovery off by default (B3).
  - Receiver at highest bandwidth, BGRA.
  - Audio ignored.
  - Reconnect on name reappearing.
  - `tools/ndi_send` (a small C++ tool on the SDK) sends a moving timecode pattern.
  - A ctest on Linux receives it on loopback and checks frame count and size changes; it is skipped when the runtime is
    absent.
- **Spout (Windows):**
  - Spout2 `SpoutReceiver` for the sender list and the shared handle.
  - `ID3D11Device1::OpenSharedResource1`, then `CopyResource` into our own texture, which is wrapped with
    `QRhiTexture::createFrom`.
  - Follow size changes; BGRA8.
  - The `tools/spout_send` CI test sends from a second process on the windows-2022 runner (WARP). If WARP cannot share
    handles, the test is marked manual.
- **Syphon (macOS):**
  - `SyphonMetalClient` on the QRhi's `MTLDevice` from `QRhi::nativeHandles()`, with no copy.
  - The `tools/syphon_send` CI test runs on macos-14; the runner has Metal.
- **Latency:** every source records arrival time against the frame it was presented in. The Media card shows latency in
  frames (NFR-3). The real measurement is the user's AT-3 run.

### B3 — Mapping view and Resolume integration (FR-15, FR-16, FR-24, AT-3)

The ground truth is the user's Arena 7.17 file (see Context). It sets every format detail below.

- **Advanced Output importer, rewritten** (`core/resolume.cpp`):
  - **Video screens:**
    - Each `<Screen>` becomes an `output`, with kind from its `OutputDevice*` and size from the device.
    - Each `<Slice>` becomes a wall. Its name is the slice's `Common/Name`.
    - The input quad becomes the centre, size and `angle` (from `orientation`). The output quad becomes `outRect`.
    - It matches an existing wall by name, then by uniqueId stored on an earlier import. Otherwise it creates a wall
      with W×H in metres from a pixel pitch the user picks in the import dialog.
  - **Pixel strips:**
    - Each `<DmxScreen>` becomes a `lumiverse` output, and each `<DmxSlice>` becomes a `pixelStrip`.
    - Pixel count and colour format come from `ParamFixturePixels`.
    - The patch comes from `Start Channel` plus the Lumiverse's base universe, via the Distribution rule.
  - **Lumiverse targets.** Arena sends each Lumiverse as Art-Net **unicast to an IP address**, the WLED controller's.
    - The import dialog lists each Lumiverse with its target IP and start universe (Net:Sub:Uni).
    - **`TargetIP` is optional.** `TT_DISABLED` (as in the sample) means no unicast target: Arena uses its default
      Art-Net output. A visualiser on the same network then receives the strips with no change in Arena.
    - When a target is set, `targetIp` is an IPv4 address and goes with `Subnet` and `Universe`.
    - All three are pre-filled from `DmxOutputParams` and can be edited in the Inspector of a Lumiverse output. Leaving
      the IP empty means `TT_DISABLED`.
    - Export writes back exactly what is set. An empty IP goes back as `TT_DISABLED`, so a file without targets round
      trips unchanged.
  - **Live preview of the strips over Art-Net.** Unicast to a WLED's IP never reaches the visualiser, so there are two
    setups, and the Media/DMX card explains both.
    - **Previz (no WLEDs):**
      - In Arena, point every Lumiverse's target IP at the visualiser PC.
      - When both run on one PC, use `127.0.0.1`. Arena's own Art-Net input must then be off, or it holds UDP 6454.
        This is checked on CI's Windows runner.
    - **On site with WLEDs:** the visualiser is a passive listener, so it needs a copy of the traffic. Use Arena's
      broadcast target, or a switch port mirror. Nothing is sent back to the WLEDs.
    - **Universes must be distinct.** The visualiser keys DMX by universe only. Lumiverses that share a universe and
      differ only by IP would overwrite each other. Two fixes:
      - the importer gives each Lumiverse its own project universe range and warns about the overlap;
      - an **input remap** (Art-Net universe → project universe) in `DmxEngine`, applied before merge, so WLEDs that all
        sit on universe 0 can still be previewed once Arena gives them distinct universes for previz.
    - **"Learn from Arena":** with Arena sending, the user solos one DMX screen, and the app records which universes
      light up and which source IP sends them.
  - **Every Arena parameter in the file can be edited** in the Inspector, under a "Resolume" section for walls, strips
    and outputs. Each is stored as typed JSON under `resolume` on the object and written back on export.
    - **Edits:** every edit goes through `Document::apply`, so it has undo.
    - **Defaults:** they come from the file's `default=` values. A value equal to its default is still written back, so
      the round trip stays byte-exact.
    - **Ranges and choices:** they come from the file's `ValueRange minMax` and the known choice lists. Unknown choices
      are shown as raw values and preserved.

    | Level | Parameters |
    |---|---|
    | Output, video (`Screen` / `OutputDeviceNDI`, Spout, display) | Name, Enabled, Hidden, Opacity, Brightness, Contrast, Red, Green, Blue, device name, width, height, Alpha Output, Bit depth |
    | Output, Lumiverse (`DmxScreen` / `OutputDeviceDmx`) | Name, Enabled, Hidden, Auto Span, Align Output, Opacity, Brightness, Contrast, Red, Green, Blue, LumiverseId, Framerate, Delay, Dmx Interface, TargetIP (optional; empty = `TT_DISABLED`), Subnet, Universe |
    | Wall slice (`Slice`) | Name, Enabled, Input Source, Input Opacity, Input Bypass/Solo, SoftEdgeEnable, input quad, angle, output quad, Flip, Brightness, Contrast, Red, Green, Blue, Is Key, Black BG, BRed, BGreen, BBlue, Warper Point Mode and Flip. The Bezier and homography points are kept and shown read-only, because Arena is the place to edit warps. |
    | Pixel strip (`DmxSlice` + fixture) | Name, Enabled, Input Source, Input Opacity, Input Bypass/Solo, Fixture profile id, Start Channel, Filter Mode, input quad, Flip, Brightness, Contrast, Red, Green, Blue, pixels Width × Height, Color Format, Distribution, Gamma |
    | Composition / global | Composition size, SoftEdging (gamma R/G/B, Gamma, Luminance, Power), screen guides (image, opacity) |

    - **Preview:** parameters that change what the LEDs show are applied in the composition-sampled preview: Opacity,
      Brightness, Contrast, RGB, Flip, Gamma, Color Format, Enabled and Hidden. The rest are stored and exported only.
      The Inspector marks these with "Arena only".
    - **Completeness check:** a test lists every `Param*` name in the sample and fails if one has no Inspector field.
      That covers all 45 names in the 7.17 file.
    - **Validation:** IPv4 format; Start Channel ≥ 1; Width and Height ≥ 1; Distribution 1–170 for RGB (128 for RGBW);
      Net, Sub-Net and Universe within Art-Net 4 ranges.
  - **Placement in 3D:**
    - Optional "**Place from composition**": the composition read as a front elevation (this file is drawn that way:
      left and right portals, gates, side walls), at a scale and stage plane the user sets.
    - Objects then sit where the drawing puts them, and the user moves them from there.
    - Without this option, new objects go into a row behind the stage.
  - **Everything else is preserved**, so the export rewrites only what we model:
    - unknown params, guides, `SoftEdging`, Warper, output params and uniqueIds;
    - slice `Input Source`. One slice in the sample uses layer input (`3:3`) and the composition preview cannot show
      it, so it gets a warning.
- **Exporter:**
  - Its output is checked byte for byte: importing the sample and exporting it again must give the same file
    (`tests/approved/` = the sample itself).
  - Projects built from scratch write the same structure:
    - screens grouped by output;
    - walls as slices, with rotation written into the quad;
    - pixel strips as `DmxScreen`/`DmxSlice` with an inline fixture.
  - Floats are written with Arena's precision.
  - Writing the Arena SVG uses the same data. It is useful as a mapping guide and nearly free.
- **Mapping screen (the Mapping tab):**
  - A `MappingItem` shows the live composition at fit or 1:1, with pan and zoom.
  - **Layers:** video slices are grouped by output, pixel strips by Lumiverse, and each group can be hidden.
  - **Editing:**
    - Slices and strips are rotatable quads you can drag. The selected one is shown in the accent colour.
    - Snapping: to pixels, to other slices' edges and to the composition edge; Alt switches it off.
    - Arrows nudge 1 px, Shift+arrows nudge 10 px.
    - Each edit is one `Document::apply`.
  - **Output side:** an "Outputs" toggle shows each processor's canvas (`outRect`) instead, to check what each NDI
    processor receives.
  - "Auto layout" packs the selected walls side by side at native resolution.
- **Pixel strips and fixtures in 3D (FR-16):**
  - With `pixelSource: dmx`, cells come from the decoded universes. This is the exact preview of what WLED will show.
  - With `slice`, cells sample the composition along the quad, on the GPU.
  - GDTF pixel fixtures can use either source too.
- **Presets folder:**
  - "Send to Resolume" writes `Documents/Resolume Arena/Presets/Advanced Output/<project>.xml`, after confirming any
    overwrite.
  - It never touches the live `AdvancedOutput.xml`, which Arena overwrites on quit.
  - Arena reads presets on load, so the user selects the preset in Arena's Advanced Output dropdown once.
- **Arena link (optional, B6):**
  - Host and port fields, plus a status dot in the title bar next to Art-Net and sACN.
  - Reads `GET /api/v1/composition` for the composition size and the layer, column and clip names.
  - Warns when Arena's composition size differs from ours. The sample's 2500×1080 must match.
  - The mapping view can trigger a column or clip for a quick look.
  - Polls at 2 Hz. Client only, so NFR-9 holds.
  - Tested against a Python mock.
- **Auto source:** an NDI, Spout or Syphon sender named like Arena is offered in the Media card with one click. The
  sample's processors already take NDI, so Arena's composition can come in over NDI on any OS.
- **Verify:**
  - The sample imports to 4 walls on 2 outputs and 41 strips on 8 Lumiverses, and the pixel and channel totals match.
  - Export reproduces the sample. Unit tests cover Distribution, rotation and quad.
  - A golden PNG of the mapping view with the sample loaded, compared with Arena's SVG.
  - A test that lights one strip over Art-Net and checks which cells change.
  - The user runs AT-3 with this show.

### B4 — MVR round trip and CSV import (FR-22, FR-23, AT-1)
- **Reader:**
  - Reads `GeneralSceneDescription.xml`: Layers, ChildList, Fixture, Truss, SceneObject, VideoScreen, GroupObject and
    the Matrix.
  - Converts the 4×3 matrix in mm with Z up to our metres with Y up and T·Ry·Rx·Rz. The Euler decomposition is tested
    with 50 random matrices.
  - Maps GDTFSpec and GDTFMode to the embedded GDTF, Addresses to the patch, FixtureID, CustomId and UnitNumber, and
    Geometry3D to glTF or 3DS.
- **Writer:**
  - The reverse, embedding every GDTF the rig uses plus walls as `VideoScreen` with glTF quads.
  - Keeps the original UUIDs and unknown XML, so import, export and import again is lossless (AT-1).
- **CSV import:**
  - A column-mapping dialog with presets for Capture and Vectorworks exports.
  - Each column maps to name, fixture ID, type, mode, universe.address and x/y/z.
- **Verify:**
  - Round-trip tests on the MVR spec's sample files and on two exports from grandMA3 and Vectorworks. **The user
    supplies those**; until then, public samples from the mvrdevelopment repo.
  - The user loads our MVR into grandMA3 for AT-1.

### B5 — FBX import and export, OBJ materials (FR-3, FR-25, AT-4)
- **Import:**
  - ufbx with unit and axis conversion (`ufbx_load_opts.target_axes` / `target_unit_meters`).
  - Per-node split into set pieces with the hierarchy kept as `parent`.
  - Materials as base colour plus texture into `geometry/`.
  - OBJ gains MTL materials.
- **Export (B2):**
  - A binary FBX 7.4 writer.
  - Hierarchy: venue, trusses, walls as named emissive quads with UVs matching their slice, fixture bodies, set
    pieces, and cameras from bookmarks.
  - Scope: the whole scene or the selection.
- **Verify:**
  - A ufbx re-read test comparing node names, transforms and vertex counts.
  - CI on ubuntu installs Blender and runs `blender -b -P tools/blender_check.py`, which imports our FBX and OBJ and
    asserts names, scale and axes. That is AT-4's Blender half automated.
  - The Vectorworks FBX half needs a sample file from the user.

### B6 — Plan and elevation drawings (FR-19)
- **Plan tab:**
  - A `DrawingItem` (QQuickPaintedItem) using `drawing_painter`, which works from scene data, not the 3D renderer.
  - It draws an orthographic plan, front or side view.
  - Fixture symbols come from type and footprint (spot, wash, beam, blinder, pixel bar), labelled with fixture ID and
    patch.
  - Truss is drawn as an outline with its length.
  - Walls appear as a line in plan and a rectangle in elevation, with W×H and resolution.
- **Dimensions:**
  - Automatic: truss trims (height above deck), the overall stage and spacing between objects in the same row.
  - Manual dimension lines are stored in `drawings`.
- **Title block:** project, sheet, scale, date and layer legend. Layers can be toggled per sheet.
- **Output:** PDF via `QPdfWriter` with the same painter, at A4/A3 and 1:50/1:100. SVG via QSvgGenerator, which comes
  free with QtSvg, already installed.
- **Verify:** golden PNGs of the demo rig plan and front elevation, and a PDF smoke test that parses the page count and
  size.

### B7 — Installers, updates, crash reports (AT-7)
- **Windows:**
  - A WiX v5 MSI, per-machine.
  - A firewall rule for UDP 6454 and 5568 inbound on private networks, plus NDI's ports.
  - Start-menu entry and `.svproj` file association.
  - windeployqt output; the NDI runtime is included only if B3 allows.
  - Signed via B7 (exe, dlls, MSI).
- **macOS:**
  - A universal (arm64 and x86_64) `.app` with hardened runtime.
  - Entitlements: `network.client` and `network.server` for UDP receive, and `cs.disable-library-validation` only if the
    NDI dylib requires it.
  - `NSLocalNetworkUsageDescription` and `NSBonjourServices` (NDI) in Info.plist.
  - macdeployqt, codesign, `notarytool submit --wait`, staple, and a DMG.
- **Updates:**
  - Sparkle 2 and WinSparkle, wrapped by `updater.cpp`.
  - The Project screen gets "Check for updates" and an opt-in toggle.
  - Release CI signs the appcast with the EdDSA key (a secret) and publishes it.
  - A **test channel** appcast lets AT-7's "update check finds a test release" run.
- **Crash reports:** Sentry Native, opt-in (B9). The About card gains licences: Qt LGPL, NDI, Spout BSD, Syphon BSD,
  Sentry MIT.
- **Verify:**
  - CI artefacts are a signed MSI and a notarised DMG once secrets exist; unsigned before.
  - The user runs AT-7 on clean Win11 and macOS 14 VMs.

### B8 — Optional: Spout, Syphon and NDI output of the 3D view (Release row; only on request, B11)
- The viewport renders to an offscreen target at a set resolution.
- The output goes out as a Spout sender, a Syphon server or an NDI sender, so Arena can use the visualiser as a source.

### Beta exit
- **Tests:** AT-1, AT-3, AT-4 and AT-7 pass as written; the user's runs are noted in the release notes.
- **Performance:** NFR-2 and NFR-3 are measured on a real GPU laptop with three live-fed walls.
- **Reviews:** `/qt-cpp-review` and `/qt-qml-review` come back clean.
- **Tag:** v0.2.0-beta.

## What the user must supply (blocking items marked ●)

| Item | Needed by | Used for |
|------|-----------|----------|
| ✓ Arena 7.17 Advanced Output file with screens and DMX strips (received; in `tests/fixtures/resolume/`) | B3 | Exact XML format and approval tests |
| Per Lumiverse (only where unicast is used): the WLED target IP and Art-Net universe, and whether the WLEDs share universe numbers | B3 | Strip patch and input remap |
| Confirmation of how Distribution 170 splits a strip across universes (one Art-Net capture from Arena is enough) | B3 | Pixel decode |
| A recording of `GET /api/v1/composition` from your Arena (optional) | B3 | REST mock |
| ● grandMA3 and Vectorworks MVR exports | B4 | AT-1 round trip |
| A Vectorworks FBX | B5 | AT-4 |
| ● Azure Trusted Signing account (or EV cert), Apple Developer ID cert and App Store Connect API key, as GitHub secrets | B7 | Signing and notarisation |
| Sparkle EdDSA key pair, Sentry DSN (optional) | B7 | Updates, crash reports |
| Test runs on Windows and macOS hardware | B2, B3, B7 | Spout, Syphon, AT-3, AT-7 |

## Risks (ranked)

1. **Spout and Syphon cannot be run here.** Mitigation: B2's CI sender tests, a `--media-selftest` flag that the user
   runs once per OS, and NDI as a fully tested path for the shared code.
2. **The Advanced Output format is undocumented.** Now reduced: a real 7.17 file is in hand, and its import and export
   round trip is the approval test. Still open:
   - Other Arena versions may differ; the importer is lenient, and a second file from 7.2x would show by how much.
   - How a strip spans universes (Distribution) is a guess until checked against Arena's actual Art-Net.
3. **Our own FBX writer is rejected by some DCC tool.** Mitigation: ufbx and Blender checks in CI. The fallback is the
   Autodesk SDK plug-in after legal review (B2).
4. **The licence terms of the NDI runtime and SDK.** Mitigation: dynamic load, user-installed runtime as the default,
   and redistribution only after review.
5. **The signing secrets do not arrive.** Mitigation: the pipeline works unsigned, and AT-7 waits on the secrets alone.
6. **MVR files from the wild** (sub-fixtures, multi-patch, 3DS geometry). Mitigation: unknown XML is preserved, so the
   round trip still holds; placeholders are logged.
7. **QtMultimedia's FFmpeg backend** differs per OS in its codecs. Mitigation: document H.264/HAP status per OS. HAP,
   which Resolume users favour, is not in Qt's FFmpeg build, so it is listed as a known limit.

## AGENTS.md updates (in B0 and as each milestone lands)

- **Section 1:** the current target becomes Beta; link this file.
- **Section 3:** add the `sv_media` row (Qt Gui and QRhi, no Quick). The render thread owns sources, and sources never
  touch the Document.
- **Section 8:** add B1–B11 once confirmed.
- **Section 9:** Beta leaves "Deferred"; Release items stay.
- **New API-in-brief sections:** media (B1–B2), mapping and Resolume (B3), MVR (B4), FBX (B5), drawings (B6), and
  release and signing (B7).
