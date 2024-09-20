lsvd_src = files(
    'backend.cc',
    'extmap.cc',
    'image.cc',
    'journal.cc',
    'main.cc',
    'read_cache.cc',
)

lsvd_deps = [
    subproject('cachelib').get_variable('cachelib_dep'),
    subproject('zpp_bits').get_variable('zppbits_dep'),

    cxx.find_library('rados', required: true),
    cxx.find_library('jemalloc', required: false),

    dependency('threads', static: true),
    dependency(
        'boost',
        modules: [
            'program_options',
        ],
        static: true,
    ),
]
