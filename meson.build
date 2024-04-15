project(
	'lsvd-rbd',
	'cpp',
	version: '0.1',
	default_options: [
		'cpp_std=c++20',
		'warning_level=2',
		'b_colorout=always',
	],
)

add_project_arguments('-fPIC', language: 'cpp')
add_project_arguments('-Wno-unused-parameter', language: 'cpp')

if get_option('buildtype') == 'debug'
	add_project_arguments('-fno-inline', language: 'cpp')
	add_project_arguments('-DLOGLV=1', language: 'cpp')

	# add sanitizers for debug builds
	add_project_arguments(
		'-fsanitize=address,undefined,nullability,implicit-conversion',
		language: 'cpp',
	)
	add_project_link_arguments(
		'-fsanitize=address,undefined',
		'-Wl,--unresolved-symbols=ignore-in-object-files',
		language: 'cpp',
	)
endif

subdir('src')
liblsvd = library('lsvd', lsvd_src, dependencies: lsvd_deps, install: false)

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
