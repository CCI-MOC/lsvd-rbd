project(
	'lsvd-rbd',
	['c', 'cpp'],
	version: '0.1',
	default_options: [
		'cpp_std=c++2a',
		'warning_level=2',
		'b_colorout=always',
		'b_sanitize=address,undefined',
		'b_lto=true',
	],
)

add_project_arguments('-fPIC', language: 'cpp')
add_project_arguments('-Wno-unused-parameter', language: 'cpp')

if get_option('buildtype') == 'debug'
	add_project_arguments('-fno-inline', language: 'cpp')
	add_project_arguments('-DLOGLV=1', language: 'cpp')
endif

subdir('src')
liblsvd = library('lsvd', lsvd_src, dependencies: lsvd_deps, install: false)

executable(
	'lsvd',
	spdk_fe,
	dependencies: lsvd_deps + [dependency('_spdk')],
)

executable(
	'imgtool',
	'src/imgtool.cc',
	link_with: liblsvd,
	dependencies: lsvd_deps,
)
executable(
	'thick-image',
	'src/thick-image.cc',
	link_with: liblsvd,
	dependencies: lsvd_deps,
)

subdir('test')