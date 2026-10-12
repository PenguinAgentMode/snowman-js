#!/usr/bin/env bash
# fetch-test262.sh: check out test262 at the pinned commit into ./test262. Every container runs it once.
set -euo pipefail
cd "$(dirname "$0")"
PIN=2e0a56762
if [ -d test262/.git ] && [ "$(git -C test262 rev-parse --short=9 HEAD)" = "$PIN" ]; then
  echo "test262 at $PIN"
  exit 0
fi
rm -rf test262
git clone -q --filter=blob:none --no-checkout https://github.com/tc39/test262.git test262
git -C test262 checkout -q "$PIN"
echo "test262 at $PIN"
