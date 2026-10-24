[Docs home](../README.md#documentation)

# Building from source

Scene Visualiser is C++20 with Qt 6.12 (Quick and QRhi). Linux is the daily development platform. CI builds and tests
Windows (MSVC) and macOS on every push.

## Prerequisites

- CMake 3.28 or newer and Ninja. `pip install cmake ninja` works without root.
- Qt 6.12.0 for your compiler, with the Multimedia, ShaderTools and Svg modules. For example:
  `pip install aqtinstall && aqt install-qt linux desktop 6.12.0 linux_gcc_64 -m qtmultimedia qtshadertools`
- A C++20 compiler: GCC 13+, Clang 17+ or MSVC 2022.
- On Linux: OpenGL and xkbcommon development headers. `xvfb-run` is needed for the render tests.

Every other dependency (Asio, nlohmann_json, pugixml, miniz, cgltf, ufbx, Catch2) is fetched by CMake.

## Build and test

```sh
export QT_ROOT_DIR=$HOME/Qt/6.12.0/gcc_64
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
./build/linux-debug/bin/SceneVisualiser
```

On Windows and macOS, use the presets listed by `cmake --list-presets`.

## Useful flags

| Flag | Effect |
|---|---|
| `--generator rainbow\|chase\|sweep\|full` | internal DMX test pattern |
| `--screen patch\|dmx\|plan\|mapping\|project\|control\|build` | start on one screen |
| `--screenshot out.png` | save the window after the first frames, then exit |
| `--mcp` | turn AI access on for this run |
| `--debug` | debug log, HUD and debug AI tools |

Helper tools:
- `tools/screenshot.sh out.png [flags]` takes a screenshot on a virtual display (Xvfb). The images in these docs were
  made that way.
- `python3 tools/dmx_send.py --proto sacn --universes 1-4 --pattern chase` sends test DMX (standard library only).

## Tests

- Catch2 unit tests cover each library: core, gdtf, protocol, render, session and mcp.
- Exporters are checked byte for byte against `tests/approved/`.
- The render is compared with `tests/golden/*.png`. After an intended visual change, regenerate the goldens with
  `SV_UPDATE_GOLDEN=1 ctest --preset linux-debug -R render_golden` and check the images before committing.

## Architecture

[AGENTS.md](../AGENTS.md) is the developer reference: module boundaries, threads, file format and per-feature APIs.
The plans are in [alpha-plan.md](alpha-plan.md) and [beta-plan.md](beta-plan.md).
