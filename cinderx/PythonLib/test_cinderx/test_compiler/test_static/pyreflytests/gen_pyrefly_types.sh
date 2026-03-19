#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="test_cinderx.test_compiler.test_static.pyreflytests."
FILTER="$1"

buck run fbcode//pyrefly/pyrefly:pyrefly -- check --report-cinderx tmp.trace "$SCRIPT_DIR"
for f in tmp.trace/types/${PREFIX}*; do
    basename="${f#tmp.trace/types/${PREFIX}}"
    if [[ "$basename" == *.test.json ]]; then
        continue
    fi
    if [[ -n "$FILTER" && "$basename" != *"$FILTER"* ]]; then
        continue
    fi
    cp "$f" "$SCRIPT_DIR/$basename"
done
rm -rf tmp.trace/
