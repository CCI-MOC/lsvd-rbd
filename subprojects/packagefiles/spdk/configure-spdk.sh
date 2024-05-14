#!/usr/bin/env bash

debug() {
  echo '===Building SPDK in debug mode...'
  ./configure --enable-debug --without-fuse --without-nvme-cuse \
			  --without-rbd --without-shared --without-xnvme
}

release() {
  echo '===Building SPDK in release mode...'
  ./configure --without-fuse --without-nvme-cuse \
			  --without-rbd --without-shared --without-xnvme
}

if [ $# -lt 1 ]; then
  echo "Usage: ./configure-spdk.sh [debug, release]"
  exit
fi

"$@"
