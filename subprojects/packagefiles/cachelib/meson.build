project('cachelib', 'cpp')

cxx = meson.get_compiler('cpp')
fs = import('fs')

csd = meson.current_source_dir()
install_dir = csd / 'opt' / 'cachelib'
lib_dir = install_dir / 'lib'
inc_dir = install_dir / 'include'

if not fs.exists(lib_dir / 'libcachelib_allocator.a')
    run_command(['contrib/build.sh', '-j'], check: true)
endif

cachelib_objs = [
    'cachelib_allocator',
    'cachelib_common',
    'cachelib_datatype',
    'cachelib_navy',
    'cachelib_shm',
]

cachelib_ext_deps = [
    cxx.find_library('double-conversion'),
    # cxx.find_library('glog'),
    # cxx.find_library('gflags'),
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
    cxx.find_library('numa'),
    # cxx.find_library('fmt'),
    dependency(
        'boost',
        modules: ['system', 'filesystem', 'program_options', 'thread', 'regex', 'context'],
        static: true,
    ),
]

cachelib_vendored_deps = [
    'async',
    'cachelib_allocator',
    'cachelib_common',
    'cachelib_datatype',
    'cachelib_navy',
    'cachelib_shm',
    'compiler',
    'compiler_ast',
    'compiler_base',
    'concurrency',
    'fizz',
    'fizz_test_support',
    'fmt',
    'folly',
    'folly_exception_counter',
    'folly_exception_tracer',
    'folly_exception_tracer_base',
    'folly_test_util',
    'follybenchmark',
    # 'gflags',
    # 'gflags_nothreads',
    'glog',
    'gmock',
    'gmock_main',
    'gtest',
    'gtest_main',
    'mustache',
    'mvfst_async_udp_socket',
    'mvfst_batch_writer',
    'mvfst_buf_accessor',
    'mvfst_bufutil',
    'mvfst_cc_algo',
    'mvfst_client',
    'mvfst_codec',
    'mvfst_codec_decode',
    'mvfst_codec_packet_number_cipher',
    'mvfst_codec_pktbuilder',
    'mvfst_codec_pktrebuilder',
    'mvfst_codec_types',
    'mvfst_constants',
    'mvfst_dsr_frontend',
    'mvfst_dsr_types',
    'mvfst_events',
    'mvfst_exception',
    'mvfst_fizz_client',
    'mvfst_fizz_handshake',
    'mvfst_flowcontrol',
    'mvfst_handshake',
    'mvfst_happyeyeballs',
    'mvfst_looper',
    'mvfst_loss',
    'mvfst_observer',
    'mvfst_qlogger',
    'mvfst_server',
    'mvfst_server_async_tran',
    'mvfst_server_state',
    'mvfst_state_ack_handler',
    'mvfst_state_datagram_handler',
    'mvfst_state_functions',
    'mvfst_state_machine',
    'mvfst_state_pacing_functions',
    'mvfst_state_simple_frame_functions',
    'mvfst_state_stream',
    'mvfst_state_stream_functions',
    'mvfst_transport',
    'mvfst_transport_knobs',
    'mvfst_transport_settings_functions',
    'mvfst_xsk',
    'rpcmetadata',
    'serverdbginfo',
    'thrift-core',
    'thriftannotation',
    'thriftanyrep',
    'thriftcpp2',
    'thriftfrozen2',
    'thriftmetadata',
    'thriftprotocol',
    'thrifttype',
    'thrifttyperep',
    'transport',
    'wangle',
    'zstd',
]

deps = []
foreach libname : cachelib_objs + cachelib_vendored_deps
    lib_dep = cxx.find_library(
        libname,
        dirs: [lib_dir],
    )
    deps += [lib_dep]
endforeach

cachelib_dep = declare_dependency(
    dependencies: deps + cachelib_ext_deps,
    include_directories: include_directories('opt' / 'cachelib' / 'include'),
)
