**English** · [Polski](../pl/ai-access.md) · [Docs home](../../README.md#documentation)

# AI access (MCP)

Scene Visualiser has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server. With it, an AI
assistant such as Claude can look at the scene and change it: add fixtures, build walls, re-patch, set light levels,
take screenshots. Every AI edit is one undo step, labelled `AI: …`.

## Turning it on

![Project screen](../images/project.png)

1. On the **Project** screen, tick **Enable AI access** under **AI ACCESS (MCP)**.
2. Optional settings:
   - **Allow LAN:** listen on all interfaces instead of only this computer. It is plain HTTP, so use it only on a show
     network or VPN.
   - **Allow file access:** enables the tools that open, import and export files by path.
3. The **Token** row shows the secret every request must carry: **Show**, **Copy**, or **Regenerate** it.
   Regenerating disconnects existing clients.

The server listens on `http://127.0.0.1:7311/mcp` by default. The title bar shows **AI ●** while it runs. The `--mcp`
flag turns it on for one run.

## Connecting Claude Code

The panel shows a ready-made `claude mcp add …` command. **Copy** it and run it on the machine where Claude Code runs.
Or add this to `.mcp.json`:

```json
{
  "mcpServers": {
    "scene-visualiser": {
      "type": "http",
      "url": "http://127.0.0.1:7311/mcp",
      "headers": { "Authorization": "Bearer <token>" }
    }
  }
}
```

Other MCP clients that speak *Streamable HTTP* work the same way.

## What it can do

- **Inspect:** `get_project`, `list_objects`, `get_patch`, `get_dmx`, `get_fixture`, `get_mapping`, `screenshot`,
  `get_drawing`, `get_recording`.
- **Build and edit:** `add_object`, `add_wall_run`, `add_fixture_array`, `save_fixture_type`, `edit_object`,
  `set_patch`, `readdress`, `delete_objects`, `undo` and `redo`.
- **Light and view:** `set_fixture`, `set_dmx`, `set_generator`, `set_camera`, `frame`, `set_screen`.
- **Files** (only with file access): `open_project`, `import_file`, `export_file`, `export_drawing`, `record`, `play`,
  `set_output`.

The client lists the full tool set when it connects.

## Debug mode

`--debug`, or the toggle on the Project screen, turns on detailed logging. It also shows a performance HUD and adds
debug tools such as `get_beams`, `get_render_stats` and `get_log`. The log file is in the app data folder, under
`logs/scene-visualiser.log`.
