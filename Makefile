.DEFAULT_GOAL := debug
.PHONY: setup setup-debug release debug paper clean

setup:
	meson setup --native-file meson.ini build-rel --buildtype=release
	meson setup --native-file meson.ini build-dbg --buildtype=debug

debug: setup
	cd build; meson compile

paper:
	@$(MAKE) -C atc2024

clean:
	cd build-rel; meson compile --clean
	cd build-dbg; meson compile --clean
