.DEFAULT_GOAL := debug
.PHONY: setup release debug clean cmake

setup:
	meson setup --native-file meson.ini build-rel --buildtype=release -Db_sanitize=none
	meson setup --native-file meson.ini build-dbg --buildtype=debug 
	ln -s build-dbg builddir

debug:
	conan install . --output-folder=conan --build=missing
	meson setup --native-file meson.ini build-dbg --buildtype=debug 
	meson compile -C build-dbg

release:
	conan install . --output-folder=conan --build=missing
	meson setup --native-file meson.ini build-rel --buildtype=release -Db_sanitize=none
	meson compile -C build-rel

clean:
	meson -C build-rel clean
	meson -C build-dbg clean

install-deps:
	# Folly deps
	sudo apt install -y libboost-all-dev libdouble-conversion-dev libevent-dev \
		libgflags-dev libgmock-dev libgoogle-glog-dev libgtest-dev \
		liblz4-dev liblzma-dev libsnappy-dev libsodium-dev libunwind-dev \
		libzstd-dev ninja-build zlib1g-dev
	# SPDK deps
	sudo apt install -y libnuma-dev libarchive-dev libibverbs-dev librdmacm-dev \
		python3-pyelftools libcunit1-dev libaio-dev nasm librados-dev librbd-dev
	# LSVD deps
	sudo apt install -y meson mold libfmt-dev librados-dev \
		libjemalloc-dev libradospp-dev pkg-config uuid-dev
	# Convenience
	sudo apt install -y fish
	# tools
	pip install conan
