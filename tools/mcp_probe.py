#!/usr/bin/env python3
"""Checks a running visualiser's AI access (MCP over HTTP) end to end: handshake, tools, a screenshot, an edit and
its undo. Stdlib only. The token comes from --token, or this machine's Scene Visualiser settings.

  python3 tools/mcp_probe.py [--url http://127.0.0.1:7311/mcp] [--token T] [--png out.png] [--wait 30] [--record]

--record also records a second of show with the 3D view, then renders half a second offline (needs file access, and the app on this machine).
"""
import argparse, base64, configparser, json, os, sys, tempfile, time, urllib.error, urllib.request


def local_token():
    config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    for path in (os.path.join(config, "SceneVisualiser", "Scene Visualiser.conf"),):
        cfg = configparser.ConfigParser(interpolation=None)
        if cfg.read(path) and cfg.has_option("General", "aiToken"):
            return cfg.get("General", "aiToken")
    return ""


class Client:
    def __init__(self, url, token):
        self.url, self.token, self.session, self.next_id = url, token, None, 1

    def post(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notify:
            msg["id"] = self.next_id
            self.next_id += 1
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                   "Authorization": f"Bearer {self.token}", "MCP-Protocol-Version": "2025-11-25"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, json.dumps(msg).encode(), headers)
        with urllib.request.urlopen(req, timeout=30) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            body = r.read()
        if notify:
            return None
        reply = json.loads(body)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def call(self, name, **args):
        result = self.post("tools/call", {"name": name, "arguments": args})
        if result.get("isError"):
            raise RuntimeError(f"{name}: {result['content'][0]['text']}")
        return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:7311/mcp")
    ap.add_argument("--token", default=None)
    ap.add_argument("--png", default=None, help="save the screenshot here")
    ap.add_argument("--wait", type=float, default=0, help="seconds to wait for the app to start listening")
    ap.add_argument("--record", action="store_true", help="record a second with the 3D view and check the mp4")
    a = ap.parse_args()
    c = Client(a.url, a.token if a.token is not None else local_token())

    deadline = time.time() + a.wait
    while True:
        if a.token is None:
            c.token = local_token()  # a first start creates it
        try:
            init = c.post("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                         "clientInfo": {"name": "mcp_probe", "version": "1"}})
            break
        except (urllib.error.URLError, ConnectionError) as e:
            if (isinstance(e, urllib.error.HTTPError) and e.code != 401) or time.time() > deadline:
                raise
            time.sleep(0.5)
    c.post("notifications/initialized", notify=True)
    print("server", init["serverInfo"], "protocol", init["protocolVersion"])

    tools = {t["name"] for t in c.post("tools/list")["tools"]}
    for need in ("get_project", "screenshot", "edit_object", "undo", "get_fixture", "set_dmx"):
        assert need in tools, f"missing tool {need}"
    print(len(tools), "tools")

    project = c.call("get_project")["structuredContent"]
    print("project", project["name"], project["counts"])

    shot = c.call("screenshot", width=640)["content"][0]
    assert shot["type"] == "image" and shot["mimeType"] == "image/png"
    png = base64.b64decode(shot["data"])
    assert png[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    if a.png:
        open(a.png, "wb").write(png)
    print("screenshot", len(png), "bytes")

    objects = c.call("list_objects", type="fixture")["structuredContent"]["objects"]
    assert objects, "no fixtures in the project"
    uuid, name = objects[0]["uuid"], objects[0].get("name", "")
    c.call("edit_object", uuid=uuid, pointer="/name", value="probe")
    assert c.call("get_object", uuid=uuid)["structuredContent"]["name"] == "probe"
    undone = c.call("undo")["structuredContent"]["undone"]
    assert undone.startswith("AI: "), undone
    assert c.call("get_object", uuid=uuid)["structuredContent"].get("name", "") == name
    print("edit and undo ok:", undone)

    fixture = c.call("get_fixture", uuid=uuid)["structuredContent"]
    print("fixture", fixture.get("patch"), "dimmer", fixture.get("decoded", {}).get("dimmer"))

    if a.record:
        base = os.path.join(tempfile.mkdtemp(), "take")
        c.call("record", path=base + ".svdmx", view=True)
        time.sleep(1.5)
        rec = c.call("record", stop=True)["structuredContent"]
        view = base + ".view.mp4"
        size = os.path.getsize(view) if os.path.exists(view) else 0
        if rec.get("error") and not size:  # no H.264 encoder in this FFmpeg build
            print("view recording skipped:", rec["error"])
        else:
            assert size > 0, f"empty {view}"
            print("view recording", size, "bytes")
            out = base + ".render.mp4"
            c.call("render_video", path=out, fps=10, duration=500, width=320, height=180)
            for _ in range(120):
                time.sleep(0.5)
                render = c.call("get_recording")["structuredContent"]["render"]
                if not render["active"]:
                    break
            assert not render["active"] and not render["error"], f"render: {render}"
            assert render["frame"] == 5 and os.path.getsize(out) > 0, f"render: {render}"
            print("offline render", os.path.getsize(out), "bytes")
    print("OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # one readable line for ctest logs
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
