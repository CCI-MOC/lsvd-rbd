project('lsvd-rbd', 'cpp', 
	version: '0.1', 
	default_options: [
		'cpp_std=c++20', 
		'buildtype=debug',
		'warning_level=2',
		'b_sanitizers=address,undefined',
		'b_colorout=always'
		])

cxx = meson.get_compiler('cpp')

lpthread = dependency('threads')
lz = dependency('zlib')
lfmt = dependency('fmt')
lboost = dependency('boost')
luring = dependency('liburing')
luuid = dependency('uuid')

lrados = cxx.find_library('rados', required: true)
ltcmalloc = cxx.find_library('tcmalloc', required: false)

libsrc = [
	'config.cc', 'download.cc', 'file_backend.cc', 'image.cc',
	'img_reader.cc', 'liblsvd.cc', 'mkcache.cc', 'nvme.cc', 'objects.cc',
	'rados_backend.cc', 'shared_read_cache.cc', 'spdk_wrap.cc', 'translate.cc',
	'write_cache.cc', 'lsvd_debug.cc']

libdeps = [lpthread, lz, lfmt, lboost, luring, lrados, luuid]

# Targets
liblsvd = library('lsvd', libsrc, dependencies: libdeps, install: false)
executable('imgtool', 'imgtool.cc', 
	link_with: liblsvd, dependencies: libdeps)
executable('thick-image', 'thick-image.cc', 
	link_with: liblsvd, dependencies: libdeps)
