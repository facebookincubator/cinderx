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
    buck_output=$(buck2 targets fbcode//cinderx/"$d"/...);
    readarray -t targets <<< "$buck_output"
    for target in "${targets[@]}"; do
      if [[ "$target" == *"_3.12" ]]; then
        buck2 build "$target";
      fi
    done
  fi
done

# Check for dynamic linking errors which may only occur when loading the module
buck2 run fbcode//cinderx:python3.12 -- -c 'import cinderx'
buck2 run @//mode/opt fbcode//cinderx:python3.12 -- -c 'import cinderx'
