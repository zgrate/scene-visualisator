#!/usr/bin/env bash
# PostToolUse hook: clang-format the C++ file Claude just edited or wrote.
f=$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))')
case "$f" in *.cpp|*.h|*.hpp) ;; *) exit 0 ;; esac
PATH="$HOME/.local/bin:$PATH" clang-format -i "$f"
