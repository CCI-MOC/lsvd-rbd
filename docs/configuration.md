# Configuring LSVD

LSVD is configured using a JSON file. When creating an image, we will
try to read the following paths and parse them for configuration options:

- Default built-in configuration
- `/usr/local/etc/lsvd.json`
- `./lsvd.json`
- user supplied path

The file read last has highest priority.

We will also first try to parse the user-supplied path as a JSON object, and if
that fails try treat it as a path and read it from a file.
