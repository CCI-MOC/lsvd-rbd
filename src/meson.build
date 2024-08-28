cxx = meson.get_compiler('cpp')

cmake = import('cmake')
cmvars = cmake.subproject_options()
cmvars.add_cmake_defines({'POSITION_INDEPENDENT_CODE': true})
concurrencpp = cmake.subproject('concurrencpp', options: cmvars)

# workaround for pic: https://github.com/mesonbuild/meson/issues/10764
concurrencpp_static = static_library(
    'concurrencpp_static',
    dependencies: concurrencpp.dependency('concurrencpp'),
    pic: true,
    install: false,
)
libconcurrencpp_satic_dep = declare_dependency(
    link_with: concurrencpp_static,
    include_directories: concurrencpp.include_directories('concurrencpp'),
)

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
    libconcurrencpp_satic_dep,
    dependency('threads', static: true),
    dependency('zlib', static: true),
    dependency('zstd', modules: ['zstd::libzstd_static'], static: true),
    dependency('libarchive', static: true),
    dependency('fmt', static: true),
    dependency(
        'boost',
        modules: ['system', 'filesystem', 'program_options', 'thread', 'regex'],
        static: true,
    ),
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
    'bdev_lsvd.cc',
    'bdev_lsvd_rpc.cc',
    'lsvd_tgt.cc',
)