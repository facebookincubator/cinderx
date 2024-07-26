#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/../

# Build everything with 3.12 except the Interpreter and Shadowcode.
for d in * ; do
  if ! [ -d "$d" ] || [[ "$d" == *Interpreter* ]] || [[ "$d" == *Shadowcode* ]]; then
    continue
  fi
  buck2 build -c cinderx.use_3_12=true fbcode//cinderx/"$d"/...
done

# Check for dynamic linking errors which may only occur when loading the module
buck2 run -c cinderx.use_3_12=true fbcode//cinderx:python -- -c 'import cinderx'
