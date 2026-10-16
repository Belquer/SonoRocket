#!/bin/sh
# Builds and runs the talkback test against a fresh local relay.
# usage: sonorocket/test/talkback/run.sh <path-to-built-rocket-relay> [builddir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); RELAY=$1; B=${2:-$HERE/build}
cmake -S "$HERE" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B" >/dev/null
"$RELAY" -sender 127.0.0.1:9100 -http 127.0.0.1:8080 -tunnel off -room talk-test > "$B/relay.log" 2>&1 &
RP=$!; trap 'kill $RP' EXIT; sleep 1
"$B/talkback_test_artefacts/Release/talkback_test" talk-test "${MIC_DELAY_MS:-0}" "${MODE:-}" | NODE_PATH="$HERE/../../tools/node_modules" node "$HERE/talk-listen.js" talk-test "$B/rec.json" > "$B/log.txt"
cat "$B/log.txt" | grep RESULT
VOICE_ECHO_MIN=${VOICE_ECHO_MIN:-$([ "${MODE:-}" = plugin ] && echo 12 || echo 15)} python3 "$HERE/check.py" "$B/log.txt" "$B/rec.json"
