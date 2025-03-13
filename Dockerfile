FROM ubuntu:24.04

# TODO(knikolla): Move (or remove) dependencies in the appropriate Makefile section
RUN apt update && apt install -y git curl build-essential sudo cmake python3 python3-pip python3-rados clang-18 lld-18

# install deps here to cache them
RUN apt install -y libboost-all-dev libdouble-conversion-dev libevent-dev \
		libgflags-dev libgmock-dev libgoogle-glog-dev libgtest-dev \
		liblz4-dev liblzma-dev libsnappy-dev libsodium-dev libunwind-dev \
		libzstd-dev ninja-build zlib1g-dev liburing-dev \
		libnuma-dev libarchive-dev libibverbs-dev librdmacm-dev \
		python3-pyelftools libcunit1-dev libaio-dev nasm librados-dev librbd-dev \
		libssl-dev libtool libncurses-dev help2man \
		meson mold libfmt-dev librados-dev libjemalloc-dev libradospp-dev \
		pkg-config uuid-dev fish

# cache cachelib build, this saves 20mins of build time
COPY subprojects /app/subprojects
WORKDIR /app/subprojects/
RUN ./get_and_build_subprojects.sh

WORKDIR /app
COPY Makefile /app/Makefile
COPY meson.* /app/
COPY src /app/src
COPY test /app/test
COPY tools /app/tools

RUN make release
ENTRYPOINT ["/app/build-rel/lsvd"]
CMD ["none", "--lsvd_cache_ram=100", "--lsvd_cache_nvm=500"]
