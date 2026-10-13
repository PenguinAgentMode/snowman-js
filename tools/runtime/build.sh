#!/usr/bin/env bash
# Builds and runs tools/runtime/core_test.cpp against the runtime and the runtime-owned built-ins (the
# interpreter's built-in files need src/interp/, so they are left out).
set -euo pipefail
cd "$(dirname "$0")/../.."
interp='function_constructor|eval|generator|async_function|async_generator|async_from_sync_iterator|promise'
builtins=$(ls src/builtins/*.cpp | grep -Ev "/($interp)\.cpp$")
g++ -std=c++20 -O1 -g -Wall -Wextra -Isrc tools/runtime/core_test.cpp src/runtime/*.cpp src/unicode/*.cpp src/regexp/*.cpp $builtins -o /tmp/core_test
/tmp/core_test
