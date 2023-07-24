### notes for first version of LSVD rewrite

The first version will implement the following functions:
```
int lsvd_read(uint64_t offset, size_t len, char *buf);
int lsvd_write(int64_t offset, size_t len, char *buf);
```
`offset` and `len` will be given in units of bytes, but are guaranteed to be multiples of 512. (note that all the mapping stuff in `extent.cc` uses units of 512-byte *sectors*)

Maximum value for `len` will probably be 1MB or less.

It should be multi-threaded - i.e. multiple threads can call `lsvd_read` and/or `lsvd_write` simultaneously.

It will use a file backend, creating files with names formatted "data.%08x" in a fixed directory. ("/tmp/lsvd"?)

deliberately omitted:
- cache. it will skip the cache, and write directly to the backend
- startup/recovery. The directory has to be empty at startup, and there won't be any attempt to make sure all the in-memory metadata makes it to disk. (i.e. the disk only lasts as long as the app testing it)
- garbage collection
- checkpoints, snapshots, any other fancy stuff
- "superblock". It won't read anything at startup - any info needed will be fixed configuration.

note - vLBA = virtual LBA, i.e. address in the virtual disk image, in units of 512B sectors.

I see the following data structures:

- Object map (vLBA -> object/offset): this tracks location of data that's been written to the backend

- Batching buffer: writes are copied here, until there's enough of them and they get written to the back end.

- batch addresses - a vector or list of vLBAs of the data currently in the batch buffer

When the batch is full, the buffer and list of addresses gets queued for a worker thread to write to the object store. (well, to /tmp/lsvd or whatever) Note that object writes will have more latency that writes to the file system - to simulate that we might want to have the file write process sleep for 5-10ms.

### read-after-write consistency

What happens if we get a write for some address, then a read for the same address before it is written to the backend? We need some way to grab it from the appropriate buffer.

One possible way:
- when we copy a write to the buffer, put the corresponding vLBA->object/offset mapping into the map. (we should know the sequence number of the object being assembled)
- keep a map M from object number to in-memory buffer, including the current batch buffer as well as any batches currently being written out by write threads
- when an object (file) write completes, remove the object number / buffer pair from M before freeing the buffer
- reads use the object map to find object/offset, then check map M to see if the object is still in memory. If it is, just copy the correct data and return; otherwise send a request for the object range.

