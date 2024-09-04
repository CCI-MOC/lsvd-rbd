folly_sub = subproject('folly')
libfolly = folly_sub.get_variable('folly_dep')

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
    'backend_rados.cc',
    'read_cache.cc',
)

lsvd_inc = include_directories('.')
lsvd_deps = [
    # libfolly,
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
    libfolly,
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