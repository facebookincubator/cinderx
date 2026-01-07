#!/bin/bash

# Generate what should be the current contents of the tests.bzl file and write
# it out to $1.

set -e

if [ -z "$1" ]; then echo "Usage: $0 <output_file>"; exit 1; fi

echo Generate 3.10
buck run fbcode//cinderx:ctl3.10 > /tmp/3.10.txt
echo Generate 3.12
buck run fbcode//cinderx:ctl3.12 > /tmp/3.12.txt
echo Generate 3.14
buck run fbcode//cinderx:ctl3.14 > /tmp/3.14.txt
echo Generate 3.15
buck run fbcode//cinderx:ctl3.15 > /tmp/3.15.txt

{
    echo "TESTS = {"
    echo \"3.10\":
    cat /tmp/3.10.txt
    echo ","
    echo \"3.12\":
    cat /tmp/3.12.txt
    echo ","
    echo \"3.14\":
    cat /tmp/3.14.txt
    echo ","
    echo \"3.15\":
    cat /tmp/3.15.txt
    echo "}"
} > "$1"
pyfmt "$1"
