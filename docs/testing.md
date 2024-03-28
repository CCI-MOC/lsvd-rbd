# Tests

The tests are all in the `test/` directory. Many of them haven't been run
in many releases and may not work.

Compile them with `make debug`

Known working tests:

- `test/test-seq.cc` - functional test, will create an image, write to it
sequentially, and read it back and check that the contents are the same as what
was written
