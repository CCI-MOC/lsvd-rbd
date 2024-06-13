# copied from xnvme's spdk subproject configuration and cleaned up a bit

project('spdk', 'c', version: '24.01')

if host_machine.system() != 'linux'
  error('Unsupported system type "@0@"'.format(exec_env))
endif

fs = import('fs')
cc = meson.get_compiler('c')

math_dep = cc.find_library('m', has_headers: ['math.h'])
ssl_dep = dependency('openssl', version: '>=1.1.1')
dlfcn_dep = cc.find_library('dl', has_headers: ['dlfcn.h'])
uuid_dep = cc.find_library('uuid', dirs: [], has_headers: ['uuid/uuid.h'])
numa_dep = cc.find_library('numa', has_headers: ['numaif.h'])
archive_dep = cc.find_library('archive', has_headers: ['archive.h'])

if get_option('build_subprojects') and not fs.exists('build')
  message('Configuring ..')
  if get_option('buildtype') == 'debug'
    run_command(['configure-spdk.sh', 'debug'], capture: true, check: true)
  else
    run_command(['configure-spdk.sh', 'release'], capture: true, check: true)
  endif
endif

if get_option('build_subprojects') and not fs.exists('build' / 'lib' / 'libspdk_nvme.a')
  message('Building ..')
  run_command([find_program('make'), '-j', '20'], capture: true, check: true, env: {})
endif

message('Setting up dependency ..')
message('build_subprojects:', get_option('build_subprojects'))

custom_libnames = [
  'spdk_event',
  'spdk_env_dpdk_rpc',
  'spdk_event_bdev',
  'spdk_bdev',
  'spdk_notify',
  # 'spdk_bdev_malloc',
  # 'spdk_bdev_null',
  # 'spdk_bdev_nvme',
  # 'spdk_bdev_passthru',
  # 'spdk_bdev_lvol',
  # 'spdk_bdev_raid',
  # 'spdk_bdev_error',
  # 'spdk_bdev_gpt',
  # 'spdk_bdev_split',
  # 'spdk_bdev_delay',
  # 'spdk_bdev_zone_block',
  'spdk_blobfs_bdev',
  'spdk_blobfs',
  'spdk_blob_bdev',
  'spdk_lvol',
  'spdk_blob',
  'spdk_nvme',
  'spdk_nvmf',
  # 'spdk_bdev_aio',
  'spdk_bdev_ftl',
  'spdk_ftl',
  'spdk_bdev_virtio',
  'spdk_virtio',
  'spdk_vfio_user',
  'spdk_event_accel',
  'spdk_accel',
  'spdk_dma',
  'spdk_accel_error',
  'spdk_accel_ioat',
  'spdk_ioat',
  'spdk_event_vmd',
  'spdk_vmd',
  'spdk_event_sock',
  'spdk_sock',
  'spdk_sock_posix',
  'spdk_event_iobuf',
  'spdk_init',
  'spdk_thread',
  'spdk_trace',
  'spdk_rpc',
  'spdk_jsonrpc',
  'spdk_json',
  'spdk_env_dpdk',
  'spdk_util',
  'spdk_log',
  'rte_eal',
  'rte_mempool',
  'rte_ring',
  'rte_mbuf',
  'rte_bus_pci',
  'rte_pci',
  'rte_mempool_ring',
  'rte_telemetry',
  'rte_kvargs',
  'rte_rcu',
  'rte_power',
  'rte_ethdev',
  'rte_vhost',
  'rte_net',
  'rte_dmadev',
  'rte_cryptodev',
  'rte_hash',
  'rte_log',
  'isal',
  'isal_crypto',
]

spdk_deps = [
  dlfcn_dep,
  math_dep,
  numa_dep,
  uuid_dep,
  ssl_dep,
  ssl_dep,
  archive_dep,
]

spdk_paths = []
foreach libname : custom_libnames
  csd = meson.current_source_dir()
  lib_dep = cc.find_library(
    libname,
    dirs: [
      csd / 'build' / 'lib',
      csd / 'dpdk' / 'build' / 'lib',
      csd / 'isa-l' / '.libs',
      csd / 'isa-l-crypto' / '.libs',
    ],
    static: true,
  )

  # Create a bunch of paths
  paths = [
    csd / 'build' / 'lib' / 'lib' + libname + '.a',
    csd / 'dpdk' / 'build' / 'lib' / 'lib' + libname + '.a',
    csd / 'isa-l' / '.libs' / 'lib' + libname + '.a',
    csd / 'isa-l-crypto' / '.libs' / 'lib' + libname + '.a',
  ]
  foreach path : paths
    if lib_dep.found() and fs.exists(path)
      spdk_paths += path
    endif
  endforeach
endforeach

spdk_inc = get_option('build_subprojects') ? include_directories('dpdk' / 'build' / 'include', 'build' / 'include') : include_directories('.')
spdk_link_args = ['-Wl,--whole-archive'] + spdk_paths + ['-Wl,--no-whole-archive']

# Construct link_args based on the above
spdk_dep = declare_dependency(
  dependencies: spdk_deps,
  link_args: spdk_link_args,
  include_directories: spdk_inc,
)