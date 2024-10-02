project(
    'lsvd-rbd',
    ['c', 'cpp'],
    version: '0.1',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2',
        'default_library=static',
        'b_colorout=always',
        'b_sanitize=address,undefined',
        'b_asneeded=false',
        'b_lto=false',
        'b_thinlto_cache=true',
        'b_lundef=false',
    ],
)

add_project_arguments('-fPIC', language: 'cpp')
add_project_arguments('-Wno-unused-parameter', language: 'cpp')
add_project_arguments('-Wno-deprecated-declarations', language: 'c')
add_project_arguments('-Wno-deprecated-declarations', language: 'cpp')
add_project_arguments('-Wno-nullability-completeness', language: 'cpp')
add_project_arguments('-Wno-defaulted-function-deleted', language: 'cpp')
# add_project_link_arguments('-static-libstdc++', language: 'cpp')
# add_project_link_arguments('-static-libgcc', language: 'cpp')
# add_project_link_arguments('-static', language: 'cpp')

if get_option('buildtype') == 'debug'
    add_project_arguments('-fno-inline', language: 'cpp')
    add_project_arguments('-DLOGLV=0', language: 'cpp')
endif

# globals
cxx = meson.get_compiler('cpp')

subdir('src')

lsvd_ar = static_library(
    'lsvd',
    lsvd_src,
    include_directories: lsvd_inc,
    dependencies: lsvd_deps,
)

executable(
    'lsvd',
    lsvd_tgt_src + lsvd_src,
    dependencies: lsvd_deps + [dependency('_spdk')],
)

executable(
    'imgtool',
    ['src/imgtool.cc'],
    link_with: lsvd_ar,
    dependencies: lsvd_deps,
)

# executable(
#     'imgtool',
#     lsvd_src + ['tools/imgtool.cc'],
#     include_directories: lsvd_inc,
#     dependencies: lsvd_deps,
# )

subdir('test')