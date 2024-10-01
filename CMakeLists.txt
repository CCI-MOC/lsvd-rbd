cmake_minimum_required(VERSION 3.28)

project(lsvd)

set(default_build_type "Debug")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD 23)
add_link_options("-fuse-ld=lld-18")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

add_compile_options(-Wall -Wextra)
# disable some annoying warnings from headers
add_compile_options(-Wno-deprecated-declarations)

# enable asan/ubsan on debug builds
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined -fno-omit-frame-pointer")

find_package(cachelib CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdk CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS outcome program_options)
find_path(ZPP_BITS_INCLUDE_DIRS "zpp_bits.h")
find_library(JEMALLOC_LIB jemalloc)
find_library(RADOS_LIB rados)

message("Imported targets: ${IMPORTED_TARGETS}")

add_executable(lsvd
    src/backend.cc
    src/extmap.cc
    src/image.cc
    src/journal.cc
    src/main.cc
    src/read_cache.cc
    src/bdev_lsvd.cc
    src/bdev_lsvd_rpc.cc
)

target_include_directories(lsvd PRIVATE ${ZPP_BITS_INCLUDE_DIRS})
target_link_libraries(lsvd PUBLIC folly)
target_link_libraries(lsvd PRIVATE
    cachelib
    fmt::fmt
    atomic
    Folly::folly Folly::folly_deps
    ${JEMALLOC_LIB}
    ${RADOS_LIB}
)
TARGET_LINK_DIRECTORIES(lsvd PRIVATE ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/spdk)
target_link_libraries(lsvd PRIVATE
    isal
    isal_crypto
    rte_bus_pci
    rte_cryptodev
    rte_dmadev
    rte_eal
    rte_ethdev
    rte_hash
    rte_kvargs
    rte_log
    rte_mbuf
    rte_mempool
    rte_mempool_ring
    rte_net
    rte_pci
    rte_power
    rte_rcu
    rte_ring
    rte_telemetry
    rte_vhost
    -Wl,--whole-archive
    SPDK::spdk_accel
    SPDK::spdk_accel_error
    SPDK::spdk_accel_ioat
    SPDK::spdk_bdev
    SPDK::spdk_bdev_aio
    SPDK::spdk_bdev_delay
    SPDK::spdk_bdev_error
    SPDK::spdk_bdev_ftl
    SPDK::spdk_bdev_gpt
    SPDK::spdk_bdev_lvol
    SPDK::spdk_bdev_malloc
    SPDK::spdk_bdev_null
    SPDK::spdk_bdev_nvme
    SPDK::spdk_bdev_passthru
    SPDK::spdk_bdev_raid
    SPDK::spdk_bdev_rbd
    SPDK::spdk_bdev_split
    SPDK::spdk_bdev_virtio
    SPDK::spdk_bdev_zone_block
    SPDK::spdk_blob
    SPDK::spdk_blob_bdev
    SPDK::spdk_blobfs
    SPDK::spdk_blobfs_bdev
    SPDK::spdk_dma
    SPDK::spdk_env_dpdk
    SPDK::spdk_env_dpdk_rpc
    SPDK::spdk_event
    SPDK::spdk_event_accel
    SPDK::spdk_event_bdev
    SPDK::spdk_event_iobuf
    SPDK::spdk_event_nvmf
    SPDK::spdk_event_sock
    SPDK::spdk_event_vmd
    SPDK::spdk_ftl
    SPDK::spdk_init
    SPDK::spdk_ioat
    SPDK::spdk_json
    SPDK::spdk_jsonrpc
    SPDK::spdk_log
    SPDK::spdk_lvol
    SPDK::spdk_notify
    SPDK::spdk_nvme
    SPDK::spdk_nvmf
    SPDK::spdk_rpc
    SPDK::spdk_sock
    SPDK::spdk_sock_posix
    SPDK::spdk_thread
    SPDK::spdk_trace
    SPDK::spdk_util
    SPDK::spdk_vfio_user
    SPDK::spdk_virtio
    SPDK::spdk_vmd
    -Wl,--no-whole-archive
    uuid
    rbd
    fuse3
)