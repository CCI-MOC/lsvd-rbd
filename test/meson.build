# test_extmap = executable(
#     'test_extentmap',
#     ['extmap_unit.cc'],
#     include_directories: lsvd_inc,
#     link_with: lsvd_ar,
#     dependencies: lsvd_deps,
# )
# test('Extentmap unit tests', test_extmap)

test_sequential_rw = executable(
    'test_seq',
    ['sequential_rw.cc'],
    include_directories: lsvd_inc,
    link_with: lsvd_ar,
    dependencies: lsvd_deps,
)

test('Sequential write/read', test_sequential_rw)
