#!/usr/bin/env bash
# Deploy a cross-built Windows exe into a runnable folder and zip it, without windeployqt (a Windows binary).
# QML modules come from qmlimportscanner (host Qt), DLLs from a recursive objdump walk over the exe and plugins.
# usage: tools/package_win.sh <build dir> <out dir>   env: QT_ROOT_DIR (target Qt), QT_HOST_PATH, LLVM_MINGW
set -euo pipefail
build=$1 out=$2
qt=${QT_ROOT_DIR:?} host=${QT_HOST_PATH:?}
objdump=${LLVM_MINGW:+$LLVM_MINGW/bin/}x86_64-w64-mingw32-objdump
dir=$out/SceneVisualiser
rm -rf "$dir" && mkdir -p "$dir"
cp "$build/bin/SceneVisualiser.exe" "$dir/"

# QML modules the app imports, copied whole (plugin DLLs and qmldir).
"$host/libexec/qmlimportscanner" -rootPath src/app/qml -importPath "$qt/qml" -importPath "$build/src/app" |
    python3 -c 'import json,sys; [print(m["path"]) for m in json.load(sys.stdin) if m.get("type")=="module" and m.get("path")]' |
    sort -u | while read -r p; do
        case $p in "$qt/qml/"*) rel=${p#"$qt/qml/"}; mkdir -p "$dir/qml/$rel"; cp -r "$p/." "$dir/qml/$rel/";; esac
    done
find "$dir/qml" -name '*d.dll' -delete -o -name '*.pdb' -delete 2>/dev/null || true
# main.cpp pins Fusion and D3D11: other Controls styles and the software-GL fallback never load.
for st in FluentWinUI3 Imagine Material Universal designer; do rm -rf "$dir/qml/QtQuick/Controls/$st"; done
rm -rf "$dir"/qml/QtQuick/Dialogs/quickimpl/qml/+{Imagine,Material,Universal}

for p in platforms/qwindows.dll styles/qmodernwindowsstyle.dll iconengines/qsvgicon.dll multimedia/ffmpegmediaplugin.dll \
         multimedia/windowsmediaplugin.dll networkinformation/qnetworklistmanager.dll tls/qschannelbackend.dll \
         imageformats/qgif.dll imageformats/qico.dll imageformats/qjpeg.dll imageformats/qsvg.dll; do
    mkdir -p "$dir/plugins/$(dirname "$p")" && cp "$qt/plugins/$p" "$dir/plugins/$p"
done

# Every DLL any binary needs that ships with Qt or the toolchain; system DLLs are not found there and stay out.
todo=$(find "$dir" -iname '*.exe' -o -iname '*.dll')
while [ -n "$todo" ]; do
    next=
    for f in $todo; do
        for d in $("$objdump" -p "$f" | sed -n 's/^\s*DLL Name: //p'); do
            [ -e "$dir/$d" ] && continue
            for src in "$qt/bin/$d" "${LLVM_MINGW:-/nonexistent}/x86_64-w64-mingw32/bin/$d"; do
                if [ -e "$src" ]; then cp "$src" "$dir/"; next="$next $dir/$d"; break; fi
            done
        done
    done
    todo=$next
done
for extra in d3dcompiler_47.dll; do [ -e "$qt/bin/$extra" ] && cp "$qt/bin/$extra" "$dir/"; done
printf '[Paths]\nPlugins = plugins\nQmlImports = qml\n' > "$dir/qt.conf"
cp COPYING COPYING.LESSER "$dir/" 2>/dev/null || true
(cd "$out" && rm -f SceneVisualiser-win64.zip && python3 -m zipfile -c SceneVisualiser-win64.zip SceneVisualiser)
echo "$out/SceneVisualiser-win64.zip"
