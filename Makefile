.DEFAULT_GOAL := debug
.PHONY: setup release debug clean cmake

setup:
	meson setup --native-file meson.ini build-rel --buildtype=release -Db_sanitize=none
	meson setup --native-file meson.ini build-dbg --buildtype=debug 
	ln -s build-dbg builddir

debug:
	meson setup --native-file meson.ini build-dbg --buildtype=debug 
	meson compile -C build-dbg

release:
	meson setup --native-file meson.ini build-rel --buildtype=release -Db_sanitize=none
	meson compile -C build-rel

clean:
	meson -C build-rel clean
	meson -C build-dbg clean

install-deps:
	sudo apt install -y libboost-all-dev libdouble-conversion-dev libevent-dev \
		libgflags-dev libgmock-dev libgoogle-glog-dev libgtest-dev \
		liblz4-dev liblzma-dev libsnappy-dev libsodium-dev libunwind-dev \
		libzstd-dev ninja-build zlib1g-dev liburing-dev \
		libnuma-dev libarchive-dev libibverbs-dev librdmacm-dev \
		python3-pyelftools libcunit1-dev libaio-dev nasm librados-dev librbd-dev \
		libssl-dev libtool libncurses-dev help2man \
		meson mold libfmt-dev librados-dev libjemalloc-dev libradospp-dev \
		pkg-config uuid-dev fish
