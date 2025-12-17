#!/bin/bash

# Verify that the tests.bzl file is up to date and accounts for all of the CPython and cinder tests.
# If the tests diff you can re-run this and cp /tmp/tests.txt to cinderx/tests.bzl

set -e

cd "$(dirname "$(readlink -f "$0")")"/../

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
} > /tmp/tests.txt
pyfmt /tmp/tests.txt

diff /tmp/tests.txt tests.bzl
