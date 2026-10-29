#!/usr/bin/env bash
# End-to-end AI access check (ctest mcp_e2e): the real app under Xvfb with --mcp, probed over HTTP by mcp_probe.py.
# Settings live in a scratch XDG_CONFIG_HOME, and port 7399 keeps clear of a running app on 7311.
# Usage: tools/mcp_e2e.sh <SceneVisualiser binary>
set -euo pipefail
app=$1
export XDG_CONFIG_HOME=$(mktemp -d) LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH="$HOME/.local/xcbcursor/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$XDG_CONFIG_HOME/SceneVisualiser"
printf '[General]\naiPort=7399\naiFiles=true\n' > "$XDG_CONFIG_HOME/SceneVisualiser/Scene Visualiser.conf"
xvfb-run -a -s "-screen 0 1280x800x24" "$app" --mcp > "$XDG_CONFIG_HOME/app.log" 2>&1 &
pid=$!
trap 'set +e; pkill -P $pid 2>/dev/null; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -rf "$XDG_CONFIG_HOME"; true' EXIT
python3 -I "$(dirname "$0")/mcp_probe.py" --url http://127.0.0.1:7399/mcp --wait 40 --record || { tail -20 "$XDG_CONFIG_HOME/app.log"; exit 1; }
