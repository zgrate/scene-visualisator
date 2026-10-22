# AI access (MCP over the network) and debug mode — plan

## Goal
You turn on **AI access** in the visualiser on your Windows PC. An AI client on any machine on the LAN (Claude Code
here on the Linux box, or Claude Desktop) connects to it. The AI can then do three things:
- **Look:** take screenshots, read the project, the patch and live DMX.
- **Act:** edit the rig, patch, set DMX values, move the camera, undo.
- **Debug:** read the log, frame timings, GPU backend, DMX loss and media latency.

Debug mode exposes these internals to you, as a HUD and a log panel, and to the AI, as extra tools.

Connecting from Claude Code:
```sh
claude mcp add --transport http visualiser http://<windows-ip>:7311/mcp --header "Authorization: Bearer <token>"
```
The token and the ready-to-copy command are on the Project screen.

## Decisions (confirm or override)
| # | Decision | Why |
|---|---|---|
| A1 | **MCP Streamable HTTP**, spec 2025-11-25: one `POST /mcp` endpoint answering `application/json`. `GET /mcp` returns 405, so there is no SSE stream. | "Over the network" rules out stdio, and the AI runs on another machine. Claude Code and Desktop both speak HTTP. Plain JSON replies avoid streaming. |
| A2 | **Our own HTTP/1.1 server on Asio,** which we already have. It is a new `sv_mcp` library: std, Asio and nlohmann, **no Qt**. | QtHttpServer is GPL/commercial and not LGPL (D3); it isn't installed either. cpp-httplib would be a new dependency for about 200 lines of code. |
| A3 | **Off by default.** When on, it binds `127.0.0.1`, or `0.0.0.0` with "Allow LAN". | The server can edit your show file and read files on disk. LAN access must be a deliberate choice. |
| A4 | **Bearer token, always required,** even on localhost. It is 32 random bytes in hex, kept in QSettings, and can be regenerated. `Origin`, when present, must be localhost or absent (DNS-rebinding rule from the MCP spec). | The spec's optional OAuth is overkill on a LAN. A token is what `claude mcp add --header` supports. |
| A5 | **All edits go through `Document::apply`,** labelled `AI: <tool>`. Every AI change is one undo step and shows in history. | Keeps the hard rule (AGENTS 3). You can always undo what the AI did. |
| A6 | **Tools run on the UI thread.** The MCP thread posts a lambda with `QMetaObject::invokeMethod(Session, …, Qt::QueuedConnection)` and waits up to 10 s on a `std::promise`, then returns a timeout error. | Session and Document are UI-thread only. The thread never blocks the UI, and a hung UI gives a clean error, not a hung client. |
| A7 | **File tools** (open, save as, import, export by path) need a second switch, "Allow file access", which is off by default. Paths are on the visualiser's machine. | Without the switch, a LAN client cannot read or write arbitrary files. Plain save to the current file is always allowed. |
| A8 | **No server push in v1.** The AI polls `get_dmx_status` and similar tools. | Push needs SSE and resource subscriptions. `// shortcut:` comment noting this; upgrade when a client needs live updates. |
| A9 | The Windows installer's firewall rule is already per program on the private profile (B7), so it covers the port with no change. | Nothing to do beyond checking it. |

## Tools (names as the AI sees them)
**Look (always available)**
- `get_project`: name, path, dirty, counts by type, universes, composition size, warnings and missing files.
- `list_objects {type?, layer?}` and `get_object {uuid}`, as the JSON the Inspector edits.
- `get_patch`: `patchRows`, `conflictCount`, and occupancy per universe as ranges.
- `get_dmx {universe}`: 512 values (merged frame), plus `get_dmx_status` (rate, loss, sources, winner).
- `get_fixture {uuid}`: decoded attributes (dimmer, pan/tilt in degrees, colour, beam angle) from the last render.
  This is what lets the AI check that "channel 7 at 255 really opens the shutter".
- `list_fixture_types`, `get_mapping` (outputs, slices, composition source).
- `screenshot {target: viewport|window, width?, view?: perspective|top|front|side|bookmark}` returns a PNG as MCP
  image content, so the AI sees what you see. It uses `grabWindow` on the UI thread, at most 1920 px wide.

**Act (each is one undo step)**
- `add_object {type, fields}`, `edit_object {uuid, pointer, value}`, `delete_objects {uuids}`, `duplicate`.
- `apply_patch {json_patch, label}`: the raw path, for edits no tool covers.
- `set_patch {uuid, "u.aaa"}`, `readdress {uuids, "u.aaa"}`, `set_fixture_type`, `select {uuids}`, `undo`, `redo`.
- `set_camera {preset | position+target}` and `set_screen {design|patch|dmx|plan|mapping|project}`.
- `set_dmx {universe, values: {address: value}}` (an internal source, merged like the others), `clear_dmx`, and
  `set_generator {pattern}`.
- `set_composition_source {kind, …}`, `save_project` (current file), and with A7 also `open/save_as/import/export`.

**Debug (only while debug mode is on)**
- `get_log {lines?, level?, category?}`: tail of an in-memory ring of the last 2000 messages.
- `get_render_stats`: fps, CPU ms for decode, build_scene and render, instance counts (boxes, beams, walls, pixels),
  RHI backend and adapter name, driver, `snapshot` revision.
- `get_system_info`: app version, OS, Qt version, network interfaces and which ones receive Art-Net or sACN, and the
  NDI runtime path.
- `get_media_status`: `mediaStatus`, latency included.

Resources: `visualiser://project.json` (read-only). Prompts: none.

## Debug mode
- **On:** with `--debug`, or the toggle on the Project screen (preference `debug`). It is live, with no restart.
- **HUD:** a small mono card at the bottom left above the dock. It shows fps and frame ms, beam and wall counts, the
  backend (D3D11, Metal, OpenGL), DMX rates and loss per active universe, media latency, and the MCP client count
  with the last tool called.
- **Log panel:** a card on the Project screen with the last 500 lines, filtered by category, with a Copy button.
- **Logging categories:** `sv.mcp`, `sv.dmx`, `sv.render`, `sv.media` and `sv.io`. Debug level is enabled only in
  debug mode; the existing file log takes everything.
- **MCP trace:** every request is logged under `sv.mcp` with the tool name, a summary of its arguments, the time in
  ms and ok or error. Screenshots are logged by size, not content.
- Not in v1 (name only): wireframe pick boxes or beam axes in the viewport, and a frame-time graph.

## UI (Project screen, new "AI ACCESS" section)
- Toggles: "Enable AI access (MCP)", "Allow LAN", "Allow file access".
- The port field (default 7311) and a status line: "Listening on 192.168.1.20:7311 · 1 client".
- The token (masked) with Copy and Regenerate.
- The `claude mcp add …` command line with a Copy button.
- The title bar shows `AI ●` next to Art-Net and sACN while a client has called within the last 10 s.
- `--mcp[=port]` turns the server on for one run, for CI and screenshots.

## Milestones
- **C0 — `sv_mcp` transport:** an Asio acceptor on its own thread.
  - HTTP/1.1 with Content-Length bodies only (chunked gets 411), a 4 MB limit and keep-alive.
  - JSON-RPC 2.0: `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`, `resources/list`,
    `resources/read`.
  - `Mcp-Session-Id` and the `MCP-Protocol-Version` header, plus the token and Origin checks.
  - The server takes a `std::function` tool table, so it has no Qt.
  - Catch2 tests: parsing, handshake, a wrong token gives 401, a bad Origin gives 403, an unknown tool gives a
    JSON-RPC error, and a batch array is rejected.
- **C1 — Look tools and screenshot:** `app/mcp_tools.cpp` builds the table over `Session`, with the A6 thread hop.
  `get_fixture` reads a decoded-state copy that the render thread publishes, mutex-guarded, at 20 Hz like the meter.
  Session tests call the table directly offscreen.
- **C2 — Act tools:** each maps to an existing Session action; `apply_patch` validates through `Document::apply` and
  returns its error. Tests cover each tool being one undo step labelled `AI: …`, and `set_dmx` showing up in
  `get_dmx`.
- **C3 — Settings and UI:** preferences, the AI ACCESS section, the title-bar dot and `--mcp`. Changing a setting
  restarts the listener.
- **C4 — Debug mode:** the log ring in the existing message handler, the categories, the HUD, the log card, the
  render stats gathered on the render thread and published like the meter, and the three debug tools.
- **C5 — End to end:**
  - `tools/mcp_probe.py` (stdlib) runs initialize, tools/list, a screenshot and an edit with undo against a running
    app under Xvfb. A ctest runs it.
  - Then a real `claude mcp add` from this machine against the app.
  - On your Windows PC: LAN access through the firewall, and a screenshot that shows D3D11 in `get_render_stats`.
  - AGENTS.md gets section 23 or 24.

## Risks
1. **Screenshots on the UI thread** cost one extra render (a few ms on a GPU, slow on llvmpipe). They are capped at
   one in flight, and a second request waits.
2. **An AI edit while you drag in the viewport:** both are on the UI thread and serialised, so the drag's
   `commit_move` lands after. Acceptable; the history shows both.
3. **Exposure on the LAN:** the token goes over plain HTTP. Fine on a show network; not for the internet. HTTPS is
   out of scope; use a VPN or SSH tunnel for remote access. The UI says so next to "Allow LAN".
4. **No live push (A8):** the AI cannot wait for "the next DMX change"; it polls.
