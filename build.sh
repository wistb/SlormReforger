#!/usr/bin/env bash
# ./build.sh          build bin/SlormReforger.dll and the bin/version.dll proxy
# ./build.sh deploy   build, then copy into the game's mods/Aurie
# ./build.sh package  build, then write dist/SlormReforger-<version>.zip
set -euo pipefail
cd "$(dirname "$0")"

# "deploy" needs the game folder: export SLORM_GAME, or put SLORM_GAME="..." in .env (untracked).
[ -f .env ] && . ./.env
GAME="${SLORM_GAME:-}"
VSWHERE="${VSWHERE:?set VSWHERE to the path of vswhere.exe}"

[ -f "$VSWHERE" ] || { echo "vswhere not found: install VS 2022 Build Tools with the C++ workload" >&2; exit 1; }
MSBUILD_WIN=$("$VSWHERE" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | tr -d '\r' | head -1)
[ -n "$MSBUILD_WIN" ] || { echo "MSBuild not found" >&2; exit 1; }

"$(wslpath -u "$MSBUILD_WIN")" "$(wslpath -w SlormReforger.vcxproj)" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64
"$(wslpath -u "$MSBUILD_WIN")" "$(wslpath -w proxy/Proxy.vcxproj)" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64

if [ "${1:-}" = deploy ]; then
	[ -d "$GAME/mods/Aurie" ] || { echo "set SLORM_GAME to the game folder (it must contain mods/Aurie)" >&2; exit 1; }
	until cp bin/SlormReforger.dll "$GAME/mods/Aurie/" 2>/dev/null; do
		echo "waiting for the game to release SlormReforger.dll..."; sleep 3
	done
	echo "deployed to $GAME/mods/Aurie"
fi

if [ "${1:-}" = package ]; then
	VERSION=$(sed -n 's/^#define SLORMREFORGER_VERSION "\(.*\)"/\1/p' source/Version.hpp)
	mkdir -p dist
	rm -f "dist/SlormReforger-$VERSION.zip"
	# Laid out to be extracted straight into the game folder.
	python3 - "dist/SlormReforger-$VERSION.zip" <<'PY'
import sys, zipfile
files = {
    "version.dll": "bin/version.dll",
    "SlormReforger-README.txt": "packaging/README.txt",
    "mods/Native/AurieCore.dll": "redist/AurieCore.dll",
    "mods/Aurie/YYToolkit.dll": "redist/YYToolkit.dll",
    "mods/Aurie/SlormReforger.dll": "bin/SlormReforger.dll",
    "mods/licenses/NOTICE.txt": "redist/NOTICE.txt",
    "mods/licenses/AGPL-3.0.txt": "LICENSE",
    "mods/licenses/DearImGui-MIT.txt": "vendor/imgui/LICENSE.txt",
}
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    for name, source in files.items():
        archive.write(source, name)
PY
	echo "wrote dist/SlormReforger-$VERSION.zip"
fi
