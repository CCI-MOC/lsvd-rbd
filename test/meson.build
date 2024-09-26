test_seq = executable(
    'test_seq',
    ['test_sequential_rw.cc'],
    include_directories: lsvd_inc,
    link_with: lsvd_ar,
    dependencies: lsvd_deps,
)

test_extmap = executable(
    'test_extentmap',
    ['test_extmap.cc'],
    include_directories: lsvd_inc,
    link_with: lsvd_ar,
    dependencies: lsvd_deps,
)

test('Extentmap unit tests', test_extmap)
test('Sequential write/read', test_seq)