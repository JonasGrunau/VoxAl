#!/usr/bin/env bash
# Builds and opens the demo host: VoxAl in a window with a dry vocal playing
# through it, out of your speakers. No DAW, no beat: just the voice.
#   ./start.sh              pick a vocal in the window
#   ./start.sh vocal.wav    your dry vocal, looped
# The vocals to pick from are listed in demo-kit.txt next to this script (see
# demo-kit.example.txt; it stays out of git), or the kit file VOC_DEMO_KIT
# names; without one there is the synthetic voice.
# CMake options for the first configure go in VOC_CMAKE_ARGS, e.g.
#   VOC_CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_JUCE=~/JUCE ./start.sh
set -euo pipefail

# A file argument is relative to where you are, not to the repo.
files=()
if [[ $# -gt 0 ]]; then
    [[ -f "$1" ]] || { echo "No such file: $1" >&2; exit 1; }
    files+=("$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
fi

cd "$(dirname "$0")"
if [[ -z "${VOC_DEMO_KIT:-}" && -f demo-kit.txt ]]; then
    export VOC_DEMO_KIT="$PWD/demo-kit.txt"
fi

# A build tree configured from another path (the repo was moved or renamed)
# points its rules there and cannot rebuild: start it over, keeping the local
# dependency checkouts it used.
deps=()
if [[ -f build/CMakeCache.txt ]]; then
    configured=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' build/CMakeCache.txt)
    if [[ ! "$configured" -ef . ]]; then
        while IFS= read -r line; do
            if [[ -d "${line#*=}" ]]; then deps+=("-D$line"); fi
        done < <(grep -E '^FETCHCONTENT_SOURCE_DIR_[A-Z0-9_]+:PATH=.' build/CMakeCache.txt || true)
        echo "build/ was configured for $configured; configuring it again here." >&2
        rm -rf build
    fi
fi

if [[ ! -f build/build.ninja ]]; then
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ${deps[@]+"${deps[@]}"} ${VOC_CMAKE_ARGS:-}
fi
cmake --build build --target voxal_demo

exec "build/voxal_demo_artefacts/Release/VoxAl Demo.app/Contents/MacOS/VoxAl Demo" ${files[@]+"${files[@]}"}

# ---------------------------------------------------------------------------
# INSTRUMENT alternative (PsyBox): no demo host; build the Standalone format
# (FORMATS VST3 AU Standalone) and open it. It shows an on-screen keyboard
# (the editor checks wrapperType_Standalone). Replace everything above from
# `files=()` on with:
#
#   cd "$(dirname "$0")"
#   BUILD_DIR=build
#   APP="$BUILD_DIR/VoxAl_artefacts/Release/Standalone/VoxAl.app"
#   if [[ "${1:-}" != "--no-build" ]]; then
#       if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
#           cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
#       fi
#       cmake --build "$BUILD_DIR" --target VoxAl_Standalone
#   fi
#   if [[ ! -d "$APP" ]]; then
#       echo "Standalone app not found at $APP. Run ./start.sh without --no-build." >&2
#       exit 1
#   fi
#   open "$APP"
#
# (Keep the moved-build-tree block from above in it too.)
