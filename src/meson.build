lsvd_src = files(
    'backend.cc',
    'extmap.cc',
    'image.cc',
    'journal.cc',
    'read_cache.cc',
)

lsvd_tgt_src = files(
    'bdev_lsvd.cc',
    'bdev_lsvd_rpc.cc',
    'bdev_noop.cc',
    'main.cc',
)

lsvd_inc = include_directories(
    '.',
)

lsvd_deps = [
    subproject('cachelib').get_variable('cachelib_dep'),
    dependency('zpp_bits'),
    dependency('concurrencpp'),
    dependency('cppcoro'),

    cxx.find_library('rados', required: true),
    cxx.find_library('jemalloc', required: false),
    cxx.find_library('atomic', required: true),

    dependency('threads', static: true),
    dependency(
        'boost',
        modules: [
            'program_options',
        ],
        static: true,
    ),
]