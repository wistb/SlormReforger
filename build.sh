#!/usr/bin/env bash
# ./build.sh          build bin/SlormReforger.dll and the bin/version.dll proxy
# ./build.sh deploy   build, then copy into the game's mods/Aurie
# ./build.sh package  build, then write dist/SlormReforger-<version>.zip
set -euo pipefail
cd "$(dirname "$0")"

GAME="${SLORM_GAME:?set SLORM_GAME to the game folder}"
VSWHERE="${VSWHERE:?set VSWHERE to the path of vswhere.exe}"

[ -f "$VSWHERE" ] || { echo "vswhere not found: install VS 2022 Build Tools with the C++ workload" >&2; exit 1; }
MSBUILD_WIN=$("$VSWHERE" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | tr -d '\r' | head -1)
[ -n "$MSBUILD_WIN" ] || { echo "MSBuild not found" >&2; exit 1; }

"$(wslpath -u "$MSBUILD_WIN")" "$(wslpath -w SlormReforger.vcxproj)" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64
"$(wslpath -u "$MSBUILD_WIN")" "$(wslpath -w proxy/Proxy.vcxproj)" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64

if [ "${1:-}" = deploy ]; then
	until cp bin/SlormReforger.dll "$GAME/mods/Aurie/" 2>/dev/null; do
		echo "waiting for the game to release SlormReforger.dll..."; sleep 3
	done
	echo "deployed to $GAME/mods/Aurie"
fi

if [ "${1:-}" = package ]; then
	VERSION=$(sed -n 's/^#define SLORMREFORGER_VERSION "\(.*\)"/\1/p' source/Version.hpp)
	mkdir -p dist
	rm -f "dist/SlormReforger-$VERSION.zip"
	python3 - "dist/SlormReforger-$VERSION.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    archive.write("bin/SlormReforger.dll", "SlormReforger.dll")
    archive.write("nexus/README.txt", "README.txt")
PY
	echo "wrote dist/SlormReforger-$VERSION.zip"
fi
