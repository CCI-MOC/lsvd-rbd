project('folly', 'cpp')

cxx = meson.get_compiler('cpp')
fs = import('fs')

csd = meson.current_source_dir()
install_dir = csd / 'builddir' / 'install'
libfolly_lib = install_dir / 'lib'

if not fs.exists(libfolly_lib / 'libfolly.a')
    run_command(['custom_build.sh'], check: true)
endif

libfolly = cxx.find_library('folly', dirs: [libfolly_lib], static: true)

folly_deps = [
    cxx.find_library('double-conversion'),
    cxx.find_library('glog'),
    cxx.find_library('gflags'),
    cxx.find_library('event'),
    cxx.find_library('z'),
    cxx.find_library('ssl'),
    cxx.find_library('crypto'),
    cxx.find_library('bz2'),
    cxx.find_library('lzma'),
    cxx.find_library('lz4'),
    cxx.find_library('zstd'),
    cxx.find_library('snappy'),
    cxx.find_library('dwarf'),
    cxx.find_library('aio'),
    cxx.find_library('uring'),
    cxx.find_library('sodium'),
    cxx.find_library('dl'),
    cxx.find_library('unwind'),
    cxx.find_library('fmt'),
    dependency(
        'boost',
        modules: ['system', 'filesystem', 'program_options', 'thread', 'regex', 'context'],
        static: true,
    ),
]

folly_dep = declare_dependency(
    dependencies: folly_deps + [libfolly],
    include_directories: include_directories('builddir/install/include'),
)