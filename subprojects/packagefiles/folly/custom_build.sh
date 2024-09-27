#!/usr/bin/env bash

set -euo pipefail

python3 ./build/fbcode_builder/getdeps.py install-system-deps --recursive
# python3 ./build/fbcode_builder/getdeps.py build --allow-system-packages

mkdir -p builddir builddir/install
cd builddir
cmake -DCMAKE_INSTALL_PREFIX:PATH=./install/ ..
cmake --build .
cmake --install .

