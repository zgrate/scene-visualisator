#!/usr/bin/env bash
# Start the app with an explicit AT-SPI bus address. Qt fails to look it up itself under Xvfb
# ("Not connected to D-Bus server"), so accessibility tools like the gui-user MCP would see no controls.
# Usage (gui-user launch_app binary): tools/a11y-launch.sh [app args]
addr=$(dbus-send --session --print-reply=literal --dest=org.a11y.Bus /org/a11y/bus org.a11y.Bus.GetAddress 2>/dev/null | tr -d ' ')
[ -n "$addr" ] && export AT_SPI_BUS_ADDRESS="$addr"
export LD_LIBRARY_PATH="$HOME/.local/xcbcursor/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$(dirname "$0")/../${SV_BUILD:-build/linux-debug}/bin/SceneVisualiser" "$@"
