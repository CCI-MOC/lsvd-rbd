cxx = meson.get_compiler('cpp')

lsvd_src = files(
	'config.cc',
	'image.cc',
	'liblsvd.cc',
	'lsvd_debug.cc',
	'mkcache.cc',
	'nvme.cc',
	'objects.cc',
	'rados_backend.cc',
	'shared_read_cache.cc',
	'spdk_wrap.cc',
	'translate.cc',
	'write_cache.cc',
)

lsvd_inc = include_directories('.')
lsvd_deps = [
	dependency('threads'),
	dependency('zlib'),
	dependency('fmt'),
	dependency('boost'),
	dependency('liburing'),
	dependency('uuid'),
	cxx.find_library('rados', required: true),
	cxx.find_library('tcmalloc', required: false),
]

spdk_fe = lsvd_src + files(
	'bdev_lsvd.cc',
	'spdk_frontend.cc',
)