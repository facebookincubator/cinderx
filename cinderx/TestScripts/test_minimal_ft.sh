#!/bin/bash

set -xe

# Go to the cinderx root
cd "$(dirname "$(readlink -f "$0")")"/../

buck run @//mode/dev :python3.14t -- -c 'print("Hello")'
buck run @//mode/dev :python3.14t-jit-all -- -c 'print("Hello")'
buck run @//mode/opt :python3.14t -- -c 'print("Hello")'
buck run @//mode/opt :python3.14t-jit-all -- -c 'print("Hello")'
