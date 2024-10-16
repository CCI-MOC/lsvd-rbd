#include "absl/status/status.h"
#include "folly/Singleton.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include <folly/Conv.h>
#include <folly/Range.h>

#include "backend.h"
#include "bdev_lsvd.h"
#include "engine.h"
#include "image.h"
#include "smartiov.h"
#include "utils.h"

using tp = std::chrono::time_point<std::chrono::high_resolution_clock>;
static std::atomic<u64> num_reqs{0};
static std::atomic<u64> total_lat_ns{0};
static std::atomic<u64> spdk_lat_ns{0};

static int bdev_lsvd_init(void);
static void bdev_lsvd_finish(void);
static int bdev_lsvd_io_ctx_size(void);

struct lsvd_bdev_io {
    spdk_thread *submit_td;
    spdk_bdev_io_status status;
    tp start;
    tp end1;
};

static spdk_bdev_module lsvd_if = {
    .module_init = bdev_lsvd_init,
    .module_fini = bdev_lsvd_finish,
    .name = "LSVD bdev module",
    .get_ctx_size = bdev_lsvd_io_ctx_size,
};
SPDK_BDEV_MODULE_REGISTER(ext_lsvd, &lsvd_if);

static int bdev_lsvd_init(void)
{
    spdk_io_device_register(
        &lsvd_if, [](auto iod, auto buf) { return 0; },
        [](auto iod, auto buf) { return; }, 0, "lsvd_poll_groups");
    return 0;
}

static void bdev_lsvd_finish(void)
{
    spdk_io_device_unregister(&lsvd_if, nullptr);
}

/**
 * Function table for the LSVD bdev module.
 */

static int lsvd_destroy_bdev(void *);
static void lsvd_submit_io(spdk_io_channel *c, spdk_bdev_io *io);
static bool lsvd_io_type_supported(void *ctx, spdk_bdev_io_type io_type);
static spdk_io_channel *lsvd_get_io_channel(void *ctx);

static const spdk_bdev_fn_table lsvd_fn_table = {
    .destruct = lsvd_destroy_bdev,
    .submit_request = lsvd_submit_io,
    .io_type_supported = lsvd_io_type_supported,
    .get_io_channel = lsvd_get_io_channel,
};

namespace
{
struct PrivateTag {
};
} // namespace

class LsvdThreadFactory : public folly::ThreadFactory
{
    constexpr static const std::string PREFIX = "LsvdTp_";

  public:
    explicit LsvdThreadFactory() : prefix_(PREFIX), suffix_(0) {}

    std::thread newThread(folly::Func &&func) override
    {
        auto name = folly::to<std::string>(prefix_, suffix_++);
        auto ret = std::thread(
            [func_2 = std::move(func), name_2 = std::move(name)]() mutable {
                // clear cpu affinity
                rte_cpuset_t all_cpuset;
                CPU_ZERO(&all_cpuset);
                auto cores = sysconf(_SC_NPROCESSORS_CONF);
                for (int i = 0; i < cores; i++)
                    CPU_SET(i, &all_cpuset);
                rte_thread_set_affinity(&all_cpuset);

                folly::setThreadName(name_2);
                func_2();
            });
        return ret;
    }

    void setNamePrefix(folly::StringPiece prefix) { prefix_ = prefix.str(); }
    const std::string &getNamePrefix() const override { return prefix_; }

  protected:
    std::string prefix_;
    std::atomic<uint64_t> suffix_;

    static folly::Singleton<LsvdThreadFactory, PrivateTag> singleton_;

  public:
    static sptr<LsvdThreadFactory> getInstance()
    {
        return singleton_.try_get();
    }
};

folly::Singleton<LsvdThreadFactory, PrivateTag> LsvdThreadFactory::singleton_;

static folly::Singleton<folly::CPUThreadPoolExecutor, PrivateTag>
    lsvd_tp_singleton([]() {
        auto tf = LsvdThreadFactory::getInstance();
        return new folly::CPUThreadPoolExecutor(8, tf);
    });

class lsvd_iodevice
{
  public:
    spdk_bdev bdev;
    uptr<LsvdImage> img;
    folly::Executor::KeepAlive<> kexe;

    lsvd_iodevice(uptr<LsvdImage> img_) : img(std::move(img_))
    {
        XLOGF(INFO, "Creating LSVD bdev iodevice {}", img->name);
        std::memset(&bdev, 0, sizeof(bdev));
        bdev.product_name = strdup("Log-structured Virtual Disk");
        bdev.name = strdup(img->name.c_str());
        bdev.blocklen = 4096;
        bdev.blockcnt = img->get_size() / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &lsvd_if;
        bdev.max_rw_size = 128 * 1024;
        bdev.fn_table = &lsvd_fn_table;

        kexe = folly::getKeepAliveToken(*lsvd_tp_singleton.try_get());
    }

    ~lsvd_iodevice()
    {
        free(bdev.product_name);
        free(bdev.name);
    }
};

static spdk_io_channel *lsvd_get_io_channel(void *ctx)
{
    lsvd_iodevice *iodev = reinterpret_cast<lsvd_iodevice *>(ctx);
    // SPDK will pass this to the iodevice's registered create/destroy
    // io_channel functions that were passed in when the device was registered.
    // We don't need to do anything special here, so just return the iodevice.
    auto ch = spdk_get_io_channel(iodev);
    assert(ch != nullptr);
    return ch;
}

static int bdev_lsvd_io_ctx_size(void) { return sizeof(lsvd_bdev_io); }

static void lsvd_io_done(lsvd_bdev_io *io, folly::Try<ResUnit> ret)
{
    auto sth = io->submit_td;
    assert(sth != nullptr);

    if (ret.hasValue() && ret->ok()) [[likely]]
        io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
    else {
        io->status = SPDK_BDEV_IO_STATUS_FAILED;
        if (ret.hasException())
            XLOGF(ERR, "IO failed with exception: {}", ret.exception().what());
        else
            XLOGF(ERR, "IO failed with error: {}", ret->status().ToString());
    }

    auto end = std::chrono::high_resolution_clock::now();
    io->end1 = end;
    auto lat1 = tdiff_ns(io->start, end);
    total_lat_ns.fetch_add(lat1);

    spdk_thread_send_msg(
        sth,
        [](void *ctx) {
            auto io = (lsvd_bdev_io *)ctx;
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), io->status);

            auto end2 = std::chrono::high_resolution_clock::now();
            auto spdk_lat = tdiff_ns(end2, io->end1);
            spdk_lat_ns.fetch_add(spdk_lat);

            if (spdk_lat > 10'000'000)
                XLOGF(WARN, "spdk lat {}ns", spdk_lat);
        },
        io);
}

static bool lsvd_io_type_supported(void *ctx, spdk_bdev_io_type io_type)
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

static void lsvd_submit_io(spdk_io_channel *c, spdk_bdev_io *io)
{
    auto start = std::chrono::high_resolution_clock::now();

    auto dev = static_cast<lsvd_iodevice *>(io->bdev->ctxt);
    auto exe = dev->kexe;
    auto &img = dev->img;
    auto lio = (lsvd_bdev_io *)(io->driver_ctx);
    lio->submit_td = spdk_io_channel_get_thread(c);
    lio->start = start;

    // io details
    auto offset = io->u.bdev.offset_blocks * io->bdev->blocklen;
    auto len = io->u.bdev.num_blocks * io->bdev->blocklen;

    // measure scheduling latency
    auto cb = [lio](auto &&r) { lsvd_io_done(lio, r); };

    switch (io->type) {
    case SPDK_BDEV_IO_TYPE_READ: {
        auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
        // dev->read(offset, iov, lio).scheduleOn(exe).start();
        img->read(offset, iov).scheduleOn(exe).start(cb);
        // img->read(offset, iov).semi().via(exe).then(cb);
        break;
    }
    case SPDK_BDEV_IO_TYPE_WRITE: {
        auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
        // dev->write(offset, iov, lio).scheduleOn(exe).start();
        img->write(offset, iov).scheduleOn(exe).start(cb);
        break;
    }
    case SPDK_BDEV_IO_TYPE_UNMAP:
        [[fallthrough]];
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        // dev->trim(offset, len, lio).scheduleOn(exe).start();
        img->trim(offset, len).scheduleOn(exe).start(cb);
        break;
    case SPDK_BDEV_IO_TYPE_FLUSH:
        // dev->flush(lio).scheduleOn(exe).start();
        img->flush().scheduleOn(exe).start(cb);
        break;
    default:
        XLOGF(ERR, "Unknown request type: {}", io->type);
        return;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto slat = tdiff_ns(start, end);
    total_lat_ns.fetch_add(slat);

    if (slat > 10'000'000)
        XLOGF(WARN, "scheduling lat {}ns", slat);

    auto old_num = num_reqs.fetch_add(1);
    if (old_num > 0 && old_num % 100000 == 0)
        XLOGF(INFO, "num {}; total {}; per iter {}ns, spdk {}ns", old_num,
              total_lat_ns.load(), total_lat_ns / old_num,
              spdk_lat_ns / old_num);
}

// Just copying from bdev_rbd, not sure where this is actually used
struct lsvd_bdev_io_channel {
    lsvd_iodevice *lsvd_dev;
    spdk_io_channel *io_channel;
};

auto bdev_lsvd_create(str pool_name, str img_name,
                      str cfg) -> Result<folly::Unit>
{
    assert(!img_name.empty());

    auto create_lsvd = [&]() -> void * {
        XLOGF(INFO, "Entering no_affinity");
        auto s3 = ObjStore::connect_to_pool(pool_name);
        if (!s3.ok()) {
            XLOGF(ERR, "Failed to connect to pool '{}'", pool_name);
            return nullptr;
        }

        auto img = LsvdImage::mount(s3.value(), img_name, cfg)
                       .scheduleOn(folly::getGlobalCPUExecutor())
                       .start()
                       .wait()
                       .value();
        if (!img.ok()) {
            XLOGF(ERR, "Failed to mount image '{}': {}", img_name,
                  img.status().ToString());
            return nullptr;
        }

        auto iodev = new lsvd_iodevice(std::move(img.value()));
        XLOGF(INFO, "Exiting no_affinity");
        return iodev;
    };
    auto iodev = static_cast<lsvd_iodevice *>(spdk_call_unaffinitized(
        [](void *ctx) -> void * {
            auto f = (decltype(create_lsvd) *)ctx;
            return (*f)();
        },
        &create_lsvd));

    if (iodev == nullptr)
        return absl::InternalError("Failed to create LSVD device");

    spdk_io_device_register(
        iodev,
        [](void *iodev, void *ctx_buf) {
            auto *ch = static_cast<lsvd_bdev_io_channel *>(ctx_buf);
            ch->lsvd_dev = static_cast<lsvd_iodevice *>(iodev);
            ch->io_channel = spdk_get_io_channel(&lsvd_if);
            return 0;
        },
        [](void *iodev, void *ctx_buf) {
            auto *ch = static_cast<lsvd_bdev_io_channel *>(ctx_buf);
            spdk_put_io_channel(ch->io_channel);
        },
        sizeof(lsvd_bdev_io_channel), img_name.c_str());

    auto err = spdk_bdev_register(&iodev->bdev);
    if (err) {
        XLOGF(ERR, "Failed to register bdev: err {}", (err));
        spdk_io_device_unregister(
            iodev, [](void *ctx) { delete (lsvd_iodevice *)ctx; });

        return absl::ErrnoToStatus(err, "Failed to register bdev");
    }

    return folly::Unit();
}

void bdev_lsvd_delete(str img_name, std::function<void(ResUnit)> cb)
{
    XLOGF(INFO, "Deleting image '{}'", img_name);
    auto rc = spdk_bdev_unregister_by_name(
        img_name.c_str(), &lsvd_if,
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

static int lsvd_destroy_bdev(void *ctx)
{
    auto iodev = reinterpret_cast<lsvd_iodevice *>(ctx);
    XLOGF(INFO, "Destroying LSVD bdev {}", iodev->bdev.name);
    std::ignore = iodev->img->unmount().scheduleOn(iodev->kexe).start().wait();
    delete iodev;
    return 0;
}
