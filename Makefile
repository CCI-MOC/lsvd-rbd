.DEFAULT_GOAL := debug
.PHONY: setup setup-debug release debug paper clean

setup:
	meson setup --native-file meson.ini build-rel --buildtype=release -Db_sanitize=none
	meson setup --native-file meson.ini build-dbg --buildtype=debug 
	ln -s build-dbg builddir

debug: setup
	cd build-dbg; meson compile

paper:
	@$(MAKE) -C atc2024

clean:
	cd build-rel; meson compile --clean
	cd build-dbg; meson compile --clean

install-deps:
	# Folly deps
	sudo apt install libboost-all-dev libdouble-conversion-dev libevent-dev \
		libflags-dev libgmock-dev libgoogle-glog-dev libgtest-dev \
		liblz4-dev liblzma-dev libsnappy-dev libsodium-dev libunwind-dev \
		libzstd-dev ninja-build zlib1g-dev
	# SPDK deps
	sudo apt install libnuma-dev libarchive-dev libibverbs-dev librdmacm-dev \
		python3-pyelftools libcunit1-dev
	# LSVD deps
	sudo apt install -y meson mold libfmt-dev librados-dev \
    	libjemalloc-dev libradospp-dev liburing-dev pkg-config uuid-dev
