#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/../ || exit

Internal/regen-cached-borrow-files.sh
if [ -n "$(sl st)" ]; then
    sl diff
    echo "Need to re-run fbcode/cinderx/Internal/regen-cached-borrow-files.sh"
    exit 1
else
    exit 0
fi
