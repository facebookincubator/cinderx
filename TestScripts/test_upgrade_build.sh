#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/../

# Build everything with 3.12 except the Interpreter and Shadowcode.
for d in * ; do
  if ! [ -d "$d" ] || [[ "$d" == *Interpreter* ]] || [[ "$d" == *Shadowcode* ]]; then
    continue
  fi
  buck2 build --local-only --no-remote-cache -c cinderx.use_3_12=true fbcode//cinderx/"$d"/...
done
