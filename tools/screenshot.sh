#!/usr/bin/env bash
# Screenshot the real app on a virtual X display (Xvfb + llvmpipe OpenGL), so agents and CI can see the UI.
# Usage: tools/screenshot.sh [out.png] [app args...]   (SV_BUILD overrides the build dir, default build/linux-debug)
set -euo pipefail
out=$(realpath -m "${1:-screenshot.png}"); shift $(( $# ? 1 : 0 ))
app="$(dirname "$0")/../${SV_BUILD:-build/linux-debug}/bin/SceneVisualiser"
# libxcb-cursor0 unpacked without sudo (apt-get download + dpkg -x); harmless if absent.
export LD_LIBRARY_PATH="$HOME/.local/xcbcursor/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
timeout 60 xvfb-run -a -s "-screen 0 1600x1000x24" "$app" --screenshot "$out" "$@"
echo "$out"
