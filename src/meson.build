cxx = meson.get_compiler('cpp')

# cmake = import('cmake')
# cmvars = cmake.subproject_options()
# cmvars.add_cmake_defines({'POSITION_INDEPENDENT_CODE': true})
# folly_cm = cmake.subproject('folly')
# libfolly = folly_cm.dependency('folly')

lsvd_src = files(
	'config.cc',
	'image.cc',
	'liblsvd.cc',
	'lsvd_debug.cc',
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
	# libfolly,
	dependency('threads'),
	dependency('zlib'),
	dependency('fmt'),
	dependency('boost', modules: ['system', 'filesystem', 'program_options', 'thread', 'regex']),
	dependency('liburing', static: true),
	dependency('uuid'),
	dependency('nlohmann_json'),
	cxx.find_library('rados', required: true),
	cxx.find_library('jemalloc', required: false),
]

spdk_fe = lsvd_src + files(
	'bdev_lsvd.cc',
	'spdk_frontend.cc',
)

lsvd_tgt = lsvd_src + files(
	'lsvd_tgt.cc',
	'bdev_lsvd.cc',
	'bdev_lsvd_rpc.cc',
)