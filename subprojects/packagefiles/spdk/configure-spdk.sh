#!/usr/bin/env bash

debug() {
  echo '===Building SPDK in debug mode...'
  ./configure --enable-debug --with-rbd --without-fuse --without-nvme-cuse \
			  --without-shared --without-xnvme
}

release() {
  echo '===Building SPDK in release mode...'
  ./configure --with-rbd --without-fuse --without-nvme-cuse \
			  --without-shared --without-xnvme
}

if [ $# -lt 1 ]; then
  echo "Usage: ./configure-spdk.sh [debug, release]"
  exit
fi

"$@"
