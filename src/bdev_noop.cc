#include "absl/status/status.h"
#include "folly/Benchmark.h"
#include "folly/BenchmarkUtil.h"
#include "folly/Executor.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "folly/executors/GlobalExecutor.h"
#include "folly/experimental/coro/Sleep.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"

#include "bdev_lsvd.h"
#include "smartiov.h"
#include "utils.h"

class NoopImage
{
  private:
    usize size;
    NoopImage(str name, usize size) : size(size), name(name) {}

  public:
    str name;

    static Task<uptr<NoopImage>> mount(str name, usize size)
    {
        co_return uptr<NoopImage>(new NoopImage(name, size));
    }

    Task<void> read(off_t offset, smartiov iovs) { co_return; }
    Task<void> write(off_t offset, smartiov iovs) { co_return; }
    Task<void> trim(off_t offset, usize len) { co_return; }
    Task<void> flush() { co_return; }

    usize get_size() { return size; }
};

static int bdev_noop_init(void);
static void bdev_noop_finish(void);
static int bdev_noop_io_ctx_size(void);

static spdk_bdev_module noop_if = {
    .module_init = bdev_noop_init,
    .module_fini = bdev_noop_finish,
    .name = "noop bdev module",
    .get_ctx_size = bdev_noop_io_ctx_size,
};
SPDK_BDEV_MODULE_REGISTER(ext_noop, &noop_if);

static int bdev_noop_init(void)
{
    spdk_io_device_register(
        &noop_if, [](auto iod, auto buf) { return 0; },
        [](auto iod, auto buf) { return; }, 0, "noop_poll_groups");
    return 0;
}

static void bdev_noop_finish(void)
{
    spdk_io_device_unregister(&noop_if, nullptr);
}

/**
 * Function table for the noop bdev module.
 */

static int noop_destroy_bdev(void *);
static void noop_submit_io(spdk_io_channel *c, spdk_bdev_io *io);
static bool noop_io_type_supported(void *ctx, spdk_bdev_io_type io_type);
static spdk_io_channel *noop_get_io_channel(void *ctx);

static const spdk_bdev_fn_table noop_fn_table = {
    .destruct = noop_destroy_bdev,
    .submit_request = noop_submit_io,
    .io_type_supported = noop_io_type_supported,
    .get_io_channel = noop_get_io_channel,
};

class noop_iodevice
{
  public:
    spdk_bdev bdev;
    uptr<NoopImage> img;
    folly::Executor::KeepAlive<> kexe;
    folly::CPUThreadPoolExecutor ctpe;

    noop_iodevice(uptr<NoopImage> img_) : img(std::move(img_)), ctpe(8)
    {
        std::memset(&bdev, 0, sizeof(bdev));
        bdev.product_name = strdup("No-op disk");
        bdev.name = strdup(img->name.c_str());
        bdev.blocklen = 4096;
        bdev.blockcnt = img->get_size() / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &noop_if;
        bdev.max_rw_size = 128 * 1024;
        bdev.fn_table = &noop_fn_table;

        kexe = folly::getKeepAliveToken(ctpe);
    }

    ~noop_iodevice()
    {
        free(bdev.product_name);
        free(bdev.name);
    }
};

static int noop_destroy_bdev(void *ctx)
{
    auto iodev = reinterpret_cast<noop_iodevice *>(ctx);
    delete iodev;
    return 0;
}

static bool noop_io_type_supported(void *ctx, spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_FLUSH:        // we only use this to ensure ordering
    case SPDK_BDEV_IO_TYPE_UNMAP:        // trim
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES: // also just trim
        return true;
    case SPDK_BDEV_IO_TYPE_RESET: // block until all pending io aborts
    default:
        return false;
    }
}

static spdk_io_channel *noop_get_io_channel(void *ctx)
{
    noop_iodevice *iodev = reinterpret_cast<noop_iodevice *>(ctx);
    // SPDK will pass this to the iodevice's registered create/destroy
    // io_channel functions that were passed in when the device was registered.
    // We don't need to do anything special here, so just return the iodevice.
    auto ch = spdk_get_io_channel(iodev);
    assert(ch != nullptr);
    return ch;
}

struct noop_bdev_io {
    spdk_thread *submit_td;
    spdk_bdev_io_status status;
};

static int bdev_noop_io_ctx_size(void) { return sizeof(noop_bdev_io); }

static void noop_io_done(noop_bdev_io *io, folly::Try<void> ret)
{
    auto sth = io->submit_td;
    assert(sth != nullptr);

    if (ret.hasValue()) [[likely]]
        io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
    else {
        io->status = SPDK_BDEV_IO_STATUS_FAILED;
        XLOGF(ERR, "IO failed with exception: {}", ret.exception().what());
    }

    spdk_thread_send_msg(
        sth,
        [](void *ctx) {
            auto io = (noop_bdev_io *)ctx;
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), io->status);
        },
        io);
}

using tp = std::chrono::time_point<std::chrono::high_resolution_clock>;
static std::atomic<u64> total_lat_us{0};
static std::atomic<u64> num_reqs{0};

static void noop_io_done(noop_bdev_io *io, tp start)
{
    auto sth = io->submit_td;
    assert(sth != nullptr);

    spdk_thread_send_msg(
        sth,
        [](void *ctx) {
            auto io = (noop_bdev_io *)ctx;
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                                  SPDK_BDEV_IO_STATUS_SUCCESS);
        },
        io);

    // spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
    //                       SPDK_BDEV_IO_STATUS_SUCCESS);

    auto now = std::chrono::high_resolution_clock::now();
    auto lat =
        std::chrono::duration_cast<std::chrono::microseconds>(now - start);
    total_lat_us.fetch_add(lat.count());
}

auto noop_task() -> Task<void>
{
    auto now = std::chrono::high_resolution_clock::now();
    folly::doNotOptimizeAway(now);
    co_return;
}

auto test_task(std::function<void(void)> f) -> Task<void>
{
    co_await noop_task();
    f();
    co_return;
}

static void noop_submit_io(spdk_io_channel *c, spdk_bdev_io *io)
{
    auto dev = static_cast<noop_iodevice *>(io->bdev->ctxt);
    auto &img = dev->img;
    auto lio = (noop_bdev_io *)(io->driver_ctx);
    lio->submit_td = spdk_io_channel_get_thread(c);

    // io details
    auto offset = io->u.bdev.offset_blocks * io->bdev->blocklen;
    auto len = io->u.bdev.num_blocks * io->bdev->blocklen;

    auto now = std::chrono::high_resolution_clock::now();
    auto old_num = num_reqs.fetch_add(1);
    if (old_num > 0 && old_num % 10000 == 0)
        XLOGF(INFO, "num {} total {} per iter {}", old_num, total_lat_us.load(),
              total_lat_us / old_num);

    auto scb = [lio, now]() { noop_io_done(lio, now); };
    test_task(scb).scheduleOn(dev->kexe).start();
    // noop_task().semi().via(exe).then(
    //     [lio, now](auto) { noop_io_done(lio, now); });

    // auto cb = [lio](auto &&r) { noop_io_done(lio, r); };
    // switch (io->type) {
    // case SPDK_BDEV_IO_TYPE_READ: {
    //     auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
    //     img->read(offset, iov).semi().via(exe).then(cb);
    //     break;
    // }
    // case SPDK_BDEV_IO_TYPE_WRITE: {
    //     auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
    //     img->write(offset, iov).semi().via(exe).then(cb);
    //     break;
    // }
    // case SPDK_BDEV_IO_TYPE_UNMAP:
    //     img->trim(offset, len).semi().via(exe).then(cb);
    //     break;
    // case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    //     img->trim(offset, len).semi().via(exe).then(cb);
    //     break;
    // case SPDK_BDEV_IO_TYPE_FLUSH:
    //     img->flush().semi().via(exe).then(cb);
    //     break;
    // default:
    //     XLOGF(ERR, "Unknown request type: {}", io->type);
    //     return;
    // }
}

// Just copying from bdev_rbd, not sure where this is actually used
struct noop_bdev_io_channel {
    noop_iodevice *noop_dev;
    spdk_io_channel *io_channel;
};

Task<void> getExecutorStats()
{
    while (true) {
        co_await folly::coro::sleep(std::chrono::seconds(5));
        auto stats = folly::getGlobalCPUExecutorCounters();
        XLOGF(INFO, "CPU Executor: {} max threads, {} active, {} tasks pending",
              stats.numThreads, stats.numActiveThreads, stats.numPendingTasks);
    }
    co_return;
}

auto bdev_noop_create(str img_name, usize size) -> ResUnit
{
    assert(!img_name.empty());

    auto img = NoopImage::mount(img_name, size)
                   .scheduleOn(folly::getGlobalCPUExecutor())
                   .start()
                   .wait()
                   .value();
    auto iodev = new noop_iodevice(std::move(img));

    // get executor stats periodically
    getExecutorStats().scheduleOn(folly::getGlobalCPUExecutor()).start();

    spdk_io_device_register(
        iodev,
        [](void *iodev, void *ctx_buf) {
            auto *ch = static_cast<noop_bdev_io_channel *>(ctx_buf);
            ch->noop_dev = static_cast<noop_iodevice *>(iodev);
            ch->io_channel = spdk_get_io_channel(&noop_if);
            return 0;
        },
        [](void *iodev, void *ctx_buf) {
            auto *ch = static_cast<noop_bdev_io_channel *>(ctx_buf);
            spdk_put_io_channel(ch->io_channel);
        },
        sizeof(noop_bdev_io_channel), img_name.c_str());

    auto err = spdk_bdev_register(&iodev->bdev);
    if (err) {
        XLOGF(ERR, "Failed to register bdev: err {}", (err));
        spdk_io_device_unregister(
            iodev, [](void *ctx) { delete (noop_iodevice *)ctx; });

        return absl::ErrnoToStatus(err, "Failed to register bdev");
    }

    return folly::Unit();
}

void bdev_noop_delete(str img_name, std::function<void(ResUnit)> cb)
{
    XLOGF(INFO, "Deleting image '{}'", img_name);
    auto rc = spdk_bdev_unregister_by_name(
        img_name.c_str(), &noop_if,
        // some of the ugliest lifetime management code you'll ever see, but
        // it should work
        [](void *arg, int rc) {
            XLOGF(INFO, "Image deletion done, rc = {}", rc);
            auto cb = (std::function<void(ResUnit)> *)arg;
            (*cb)(errcode_to_result(rc));
            delete cb;
        },
        new std::function<void(ResUnit)>(cb));

    if (rc != 0) {
        XLOGF(ERR, "Failed to delete image '{}': {}", img_name, rc);
        cb(absl::ErrnoToStatus(rc, "Failed to delete image"));
    }
}
