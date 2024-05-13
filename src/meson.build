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

spdk_fe = lsvd_src + files('spdk_frontend.cc')

spdk_deps = [
	dependency('numa'),
	cxx.find_library('archive'),
]

spdk_libs = [
	dependency('spdk_bdev', static: true),
	dependency('spdk_env_dpdk', static: true),
	dependency('spdk_json', static: true),
	dependency('spdk_nvme', static: true),
	dependency('spdk_nvmf', static: true),
	dependency('spdk_rpc', static: true),
	dependency('spdk_trace', static: true),
	dependency('spdk_util', static: true),
]