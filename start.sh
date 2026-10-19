#!/usr/bin/env bash
# Builds and opens the demo host: Vocanal in a window with a dry vocal playing
# through it over a psytrance loop, out of your speakers. No DAW.
#   ./start.sh                        the synthetic voice (pick a scenario in the window)
#   ./start.sh vocal.wav [beat.wav]   your dry vocal (and beat), looped
# The built-in loop uses real samples when demo-kit.txt lies next to this
# script (see demo-kit.example.txt; it stays out of git), or VOC_DEMO_KIT
# names a kit file; otherwise a synthesized loop.
# CMake options for the first configure go in VOC_CMAKE_ARGS, e.g.
#   VOC_CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_JUCE=~/JUCE ./start.sh
set -euo pipefail

# File arguments are relative to where you are, not to the repo.
files=()
for f in "$@"; do
    [[ -f "$f" ]] || { echo "No such file: $f" >&2; exit 1; }
    files+=("$(cd "$(dirname "$f")" && pwd)/$(basename "$f")")
done

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
cmake --build build --target vocanal_demo

exec "build/vocanal_demo_artefacts/Release/Vocanal Demo.app/Contents/MacOS/Vocanal Demo" ${files[@]+"${files[@]}"}

# ---------------------------------------------------------------------------
# INSTRUMENT alternative (PsyBox): no demo host; build the Standalone format
# (FORMATS VST3 AU Standalone) and open it. It shows an on-screen keyboard
# (the editor checks wrapperType_Standalone). Replace everything above from
# `files=()` on with:
#
#   cd "$(dirname "$0")"
#   BUILD_DIR=build
#   APP="$BUILD_DIR/Vocanal_artefacts/Release/Standalone/Vocanal.app"
#   if [[ "${1:-}" != "--no-build" ]]; then
#       if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
#           cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
#       fi
#       cmake --build "$BUILD_DIR" --target Vocanal_Standalone
#   fi
#   if [[ ! -d "$APP" ]]; then
#       echo "Standalone app not found at $APP. Run ./start.sh without --no-build." >&2
#       exit 1
#   fi
#   open "$APP"
#
# (Keep the moved-build-tree block from above in it too.)
