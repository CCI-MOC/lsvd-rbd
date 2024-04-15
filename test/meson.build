seq = executable(
	'test-seq',
	'test-seq.cc',
	include_directories: lsvd_inc,
	link_with: liblsvd,
	dependencies: lsvd_deps,
)
rados = executable(
	'test-rados',
	'test-rados.cc',
	include_directories: lsvd_inc,
	link_with: liblsvd,
	dependencies: lsvd_deps,
)
unit1 = executable(
	'unit-test',
	'unit-test.cc',
	include_directories: lsvd_inc,
	link_with: liblsvd,
	dependencies: lsvd_deps,
)

# executable('lsvd_crash_test', 'lsvd_crash_test.cc', include_directories: lsvd_inc, link_with: liblsvd, dependencies: lsvd_deps)
# executable('lsvd_rnd_test', 'lsvd_rnd_test.cc', include_directories: lsvd_inc, link_with: liblsvd, dependencies: lsvd_deps)

test('Sequential write/read', seq)
test('Misc unit tests', unit1)
test('RADOS performance test?', rados)
