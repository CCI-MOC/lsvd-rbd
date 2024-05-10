seq = executable(
	'test-seq',
	'test-seq.cc',
	include_directories: lsvd_inc,
	link_with: liblsvd,
	dependencies: lsvd_deps,
)

extentmap = executable(
	'test-extentmap',
	'test-extentmap.cc',
	include_directories: lsvd_inc,
	link_with: liblsvd,
	dependencies: lsvd_deps,
)

# executable('lsvd_crash_test', 'lsvd_crash_test.cc', include_directories: lsvd_inc, link_with: liblsvd, dependencies: lsvd_deps)

test('Sequential write/read', seq)
# test('Misc unit tests', extentmap)
