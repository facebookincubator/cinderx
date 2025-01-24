#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/../

# Build everything with 3.12 except the Interpreter and Shadowcode.
for d in * ; do
  if ! [ -d "$d" ] || [[ "$d" == *Interpreter* ]] || [[ "$d" == *Shadowcode* ]]; then
    continue
  elif [[ "$d" == *PythonBin* ]]; then
    # for pythonbin, make sure 3.10 and 3.12 dev builds work
    buck2 build fbcode//cinderx/PythonBin:python_3.12
    buck2 build fbcode//cinderx/PythonBin:python_3.10
  else
    buck2 build -c cinderx.use_3_12=true fbcode//cinderx/"$d"/...
  fi
done

# Check for dynamic linking errors which may only occur when loading the module
buck2 run -c cinderx.use_3_12=true fbcode//cinderx:python -- -c 'import cinderx'
buck2 run @//mode/opt -c cinderx.use_3_12=true fbcode//cinderx:python -- -c 'import cinderx'
