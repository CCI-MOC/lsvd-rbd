lpthread = dependency('threads')
lz = dependency('zlib')
lfmt = dependency('fmt')
lboost = dependency('boost')
luring = dependency('liburing', static: true)
luuid = dependency('uuid')

cxx = meson.get_compiler('cpp')
lrados = cxx.find_library('rados', required: true)
ltcmalloc = cxx.find_library('tcmalloc', required: false)

lsvd_src = files(
	'config.cc', 'download.cc', 'file_backend.cc', 'image.cc',
	'img_reader.cc', 'liblsvd.cc', 'mkcache.cc', 'nvme.cc', 'objects.cc',
	'rados_backend.cc', 'shared_read_cache.cc', 'spdk_wrap.cc', 'translate.cc',
	'write_cache.cc', 'lsvd_debug.cc')

lsvd_inc = include_directories('.')
lsvd_deps = [lpthread, lz, lfmt, lboost, luring, lrados, luuid]

liblsvd = library('lsvd', lsvd_src, dependencies: lsvd_deps, install: false)

executable('imgtool', 		'imgtool.cc', 		link_with: liblsvd, dependencies: lsvd_deps)
executable('thick-image', 	'thick-image.cc', 	link_with: liblsvd, dependencies: lsvd_deps)
