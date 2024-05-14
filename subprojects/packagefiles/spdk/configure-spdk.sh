#!/usr/bin/env bash

debug() {
  echo '===Building SPDK in debug mode...'
  ./configure --enable-debug --without-shared
}

release() {
  echo '===Building SPDK in release mode...'
  ./configure --without-shared
}

if [ $# -lt 1 ]; then
  echo "Usage: ./configure-spdk.sh [debug, release]"
  exit
fi

"$@"
