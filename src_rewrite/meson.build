lsvd_src = files(
    'backend.cc',
    'extmap.cc',
    'image.cc',
    'journal.cc',
)

lsvd_deps = [
    subproject('folly').get_variable('folly_dep'),
    subproject('liburing').get_variable('uring'),
    subproject('zpp_bits').get_variable('zppbits_dep'),

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
    dependency('nlohmann_json'),
    cxx.find_library('rados', required: true),
    cxx.find_library('rbd', required: true),
    cxx.find_library('jemalloc', required: false),
]

spdk_fe = lsvd_src + files(
    'lsvd_main.cc',
)

lsvd_tgt = lsvd_src + files(
    'lsvd_main.cc',
)