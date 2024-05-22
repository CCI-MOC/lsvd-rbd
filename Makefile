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
	sudo apt install -y meson mold libfmt-dev libaio-dev librados-dev \
    	libjemalloc-dev libboost-all-dev libradospp-dev \
    	liburing-dev pkg-config uuid-dev libnuma-dev libarchive-dev \
		libibverbs-dev librdmacm-dev python3-pyelftools libcunit1-dev

