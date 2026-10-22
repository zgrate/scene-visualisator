#!/usr/bin/env bash
# Stop hook: build and run tests before Claude finishes; failures go back to Claude (exit 2) to fix.
python3 -c 'import json,sys; sys.exit(1 if json.load(sys.stdin).get("stop_hook_active") else 0)' || exit 0
cd "$(dirname "$0")/../.." || exit 0
[ -d build/linux-debug ] || exit 0
export PATH="$HOME/.local/bin:$PATH"
if ! out=$( { cmake --build --preset linux-debug && ctest --preset linux-debug; } 2>&1 ); then
    echo "$out" | tail -40 >&2
    exit 2
fi
