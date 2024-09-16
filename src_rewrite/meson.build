folly_sub = subproject('folly')
libfolly = folly_sub.get_variable('folly_dep')

lsvd_src = files(
    'backend.cc',
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
    subproject('liburing').get_variable('uring'),
    subproject('cereal').get_variable('cereal_dep'),
    dependency('uuid'),
    dependency('nlohmann_json'),
    cxx.find_library('rados', required: true),
    cxx.find_library('rbd', required: true),
    cxx.find_library('jemalloc', required: false),
    libfolly,
]

spdk_fe = lsvd_src + files(
    'lsvd_main.cc',
)

lsvd_tgt = lsvd_src + files(
    'lsvd_main.cc',
)