#!/usr/bin/env bash
# ./build.sh          build bin/SlormReforger.dll
# ./build.sh deploy   build, then copy into the game's mods/Aurie
set -euo pipefail
cd "$(dirname "$0")"

GAME="${SLORM_GAME:?set SLORM_GAME to the game folder}"
VSWHERE="${VSWHERE:?set VSWHERE to the path of vswhere.exe}"

[ -f "$VSWHERE" ] || { echo "vswhere not found: install VS 2022 Build Tools with the C++ workload" >&2; exit 1; }
MSBUILD_WIN=$("$VSWHERE" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | tr -d '\r' | head -1)
[ -n "$MSBUILD_WIN" ] || { echo "MSBuild not found" >&2; exit 1; }

"$(wslpath -u "$MSBUILD_WIN")" "$(wslpath -w SlormReforger.vcxproj)" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64

if [ "${1:-}" = deploy ]; then
	until cp bin/SlormReforger.dll "$GAME/mods/Aurie/" 2>/dev/null; do
		echo "waiting for the game to release SlormReforger.dll..."; sleep 3
	done
	echo "deployed to $GAME/mods/Aurie"
fi
