# Log-structured virtual disk

The main things to describe are:
- block-over-object translation layer (including GC)
- read and write cache (on local SSD / file system)
- consistency model
- "superblock"
- snapshot and clone mechanisms
- future extensions: shared cache for cloned images, defragmentation
- code layout
- extent map data structure

## block-over-object translation layer

Why? (a) because it works over S3, (b) because object create / read / delete over erasure-coded RADOS objects is faster / more efficient than small mutations to triple-replicated ones.

The backend format is a stream of numbered objects, with each containing data blocks and a header identifying the LBAs stored in each block - conceptually similar to the ext3/4 journal format.
An in-memory map at the client maps each LBA to its most recent location, identified as an object # / offset pair; we use an extent map for efficiency and do a few tricks to keep it from getting fragmented.

In theory you could mount a volume by reading headers from object 0 forward, updating the map with each header; when you get to the last one, you've got an up-to-date map and are good to go.
That doesn't scale, so we periodically write metadata objects holding a full map; crash recovery starts with the last full map and then looks at headers of all objects newer than that.

Random write performance is very good, as we just batch the writes into fairly large objects - 8MB in the current implementation - and write them to an EC pool for better write throughput.
In theory read performance could suffer due to fragmentation from random writes; however a combination of local caching and defragmentation logic in the GC helps avoid that.

Garbage collection is similar to flash, except after copying live data out of old objects we just delete them, as there's no need to erase and recycle them. 
GC is generational - i.e. it doesn't mix new writes with old data - which helps reduce write amplification.
(i.e. a lot of blocks will be overwritten or trimmed before an object is GCed; the remaining blocks are longer-lived, and will be copied into objects with other long-lived blocks)

## read and write cache

The caches live in the local file system, and should be backed by NVME flash.
We currently use libaio with direct I/O, although we'll probably want to switch to io\_uring at some point.
The write cache is log-structured - like the backend, each record has a header listing the LBAs in the record, followed by the data.
Note that the sequential write pattern should significantly reduce write amplification and extend the SSD life.
Finally, under heavy small-write load we pack multiple client writes into a single record, which gives us a significant speedup for 4K writes. (I'm not sure yet how much is kernel vs SSD per-operation overhead)
There's a separate map for the write cache; when a write to cache completes we update the map and signal completion of the I/O, and for reads we check the write map before forwarding to the translation layer.

Like the object log, the map can be recovered by reading log record headers from oldest to newest and creating the map.
Again like the object log, we periodically serialize the entire map and save it to a separate location in the cache, which makes the recovery process a bit quicker - load the out-of-date map plus a pointer to the log head when it was saved, and then read forward from there.
Log records include sequence numbers and header+data CRCs to reliably detect the end of the log.

The read cache is (logically) part of the translation layer, and is indexed by object # / offset rather than LBA.
This has two benefits: (a) it's easy to do larger reads, which gives us prefetching and reduces the number of back-end object reads, and (b) it avoids consistency problems from "stale" read data.

For real workloads, fetching larger blocks (currently 64K) is a win; however some joker is always going to run `fio` random read and break things.
To handle that we limit the volume of "wasted" cache fills, and start sending reads directly to the backend when the cache is performing poorly.

## Consistency model

Consistency is based on preserving write order in the write cache and the backend.

First, we find the most recent "safe" object in the backend.
In particular, since we can have a window of outstanding writes, there's no guarantee which will have completed if we crash - e.g. if we crash while writing 2, 3, and 4, we might only find 2 and 4 after a crash.
We take the longest "prefix" - in this case up through object 2, since the writes in object 4 follow the lost writes in object 3.

If the client machine (i.e. VM host) comes back up along with its SSD, then we recover the local journal to get all the writes newer than object 2, and write them out to new objects 3, 4, and possibly 5 if there were any writes we hadn't started streaming to the backend yet, then we're up and running, with every write that was signaled complete at the RBD interface.

If we lost the SSD or client machine, we just go with object 2.
At this point we have a consistent disk image, as it represents all writes up through the last one stored in 2, and no following writes.
At worst we've lost a few seconds of writes, which sucks if you have a single-machine database, but works pretty well with most replicated services.
In particular, most services (e.g. Raft, as in etcd) will ask their peers for all the transactions they missed, **using the last transaction logged in their file system to identify when they went down**.
This means that instead of seeing the 3-minute reboot time typical of my servers, Raft will see a 3 minute and a few seconds reboot time.

## "superblock"

For an image named "abc", data and checkpoints will be stored in abc.001, abc.002 etc. (actually "abc.%08x", although maybe it should be "abc.%016x")
The object named "abc" will contain image-level data like volume size, UUID, and other stuff - more on this in the snapshot/clone section.
In particular, every time we write a checkpoint object we re-write the superblock object with a pointer indicating the sequence number of the checkpoint.
This is especially useful for RADOS, as it avoids the need to enumerate all the objects in the pool (which probably contains multiple images) in order to find the "tail" of the object stream - once we know where to start, we can just look for increasing object numbers until we don't find any more.

## snapshot and clone mechanisms

Since the object stream is log structured, a snapshot is basically just a pointer to the most recent object at the point in time being snapshotted.
There are a few slightly tricky parts:

1. all writes that were completed (i.e. written to local write cache) at the time of the snapshot have to be flushed out to the object stream, and we need to wait until the object writes have completed.
2. The snapshot has to be recorded somewhere - there's a section in the superblock for listing snapshots.
3. We have to prevent data needed by the snapshot from being GC'ed, and then ensure that it *does* get GC'ed after the snapshot is deleted.

The third point is handled by running GC normally, but deferring deletion of any objects that are "pinned" by snapshots.
This gives us a list of "deferred deletes"; whenever we delete a snapshot we go through the list and delete any that were only pinned by the snapshot being deleted.

Clones are just a naming trick.
In particular, say you have an image with object stream "base.000" ... "base.025".
Then we can create a clone object with object stream "clone.026", "clone.027" etc., and a rule for translating from object number to object name that uses "base.%03d" for object numbers less than 26, and "clone.%03d" for any higher-numbered ones. (well, it's "%08x", but anyway...)

## Future extensions

There are two extensions we want to add, but haven't prototyped yet:

Defragmentation - there's no reason why multiple map entries can't point to the same object/offset, and this can be used for defrag.
In particular, we plan to make a tool which takes a clone base image and reprocesses it, deduping all the blocks and creating a new image with a map that "reconstitutes" the original image from the deduped one.
We'll have to think a bit about how much read fragmentation this produces, and might be able to get a paper out of the tradeoff between read fragmentation and dedup compression :-)

Shared cache - if multiple virtual disks are cloned from a single base image, there's no need to cache multiple copies of the base image data.
We're thinking it should be possible to have a daemon process which handles caching the base image, and make the cache file accessible read-only to each of the virtual disks, but we haven't put a lot of thought into it yet.

## code layout

At the moment the code is divided into (a) translation layer, (b) read cache, (c) write cache, and (d) RBD API layer, which coordinates.
Currently there are separate object instances at each layer: RBD image, write cache, read cache and translation layer.
Plus there's a map (LBA -> object/offset) shared by the read cache and the translation layer, because the code structure is gross.

Writes are sent by the RBD API to the write cache; when they're durable, the write completes back to the API and is also copied to the translation layer, where it is batched and sent out to the backend.
Reads are checked against the write cache, and remaining "holes" are sent to the read cache.
If there's a local cached copy the read is completed from it; otherwise a block of data is fetched, the read is completed, and the block written to read cache.
(well, unless the read cache is performing poorly, in which case we just send it directly to the backend)

This is mostly done with async operations.
The current implementation is kind of a mess, using lambda-based continuations which get tossed around willy-nilly.
We're in the middle of changing it to use request objects which encapsulate the state and logic for a request at a particular layer, and then invoke a `notify()` interface at the layer above when they complete.
I haven't been able to fully understand the Ceph async request mechanism, but this seems like it should be much easier to rewrite into idiomatic Ceph code than the continuation stuff would be.

Update: all closures have been converted, although some of the comments need to be fixed. (i.e. it doesn't matter that certain state corresponds to a closure that used to be on line X)

## extent map

A central part of the code is the extent map, which does a pretty efficient job (wrt to both space and time) of mapping from LBA ranges to object locations, other LBAs, or pointers within a buffer.
It's a bunch of C++ template code, and is probably the most polished part of the code we have at the moment. (although the iterator isn't great, as I was still learning modern C++)

The organization is stolen from Python sortedcontainers - it's actually a 2d array rather than a tree.
It's a lot more compact - no per-element pointers, just some wasted elements in the 2nd-level arrays.
It's also quite fast on modern CPUs, as it doesn't do pointer chasing and the CPU prefetcher works really well.
The inner templates are a bit gross in order to pack the data structures better, which helps both space and time.

## status

It's been working with the fio rbd backend all along.

As of 10/27 on QEMU/KVM it makes it almost all the way through installation from an old Ubuntu 14 ISO I had sitting around.
