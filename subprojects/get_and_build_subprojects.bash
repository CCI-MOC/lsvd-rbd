#!/usr/bin/env bash
set -xeuo pipefail

# call this script in docker to build subprojects so we can cache them

# spdk
git clone -b v24.09 https://github.com/spdk/spdk.git --depth=1 --recurse-submodules
cp packagefiles/spdk/* spdk/
cd spdk
./configure-spdk.sh release
make -j $(nproc)
cd ..

# cachelib
git clone -b v20240621 https://github.com/facebook/cachelib.git --depth=1
cp packagefiles/cachelib/* cachelib/
cd cachelib
git apply ./*.patch
./contrib/build.sh -j
