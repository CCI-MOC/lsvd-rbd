#include "absl/status/status.h"
#include "fmt/format.h"
#include "folly/Conv.h"
#include "folly/Executor.h"
#include "folly/Range.h"
#include "folly/Singleton.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "numa.h"
#include "rte_os.h"
#include "rte_thread.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include <sched.h>

#include "backend.h"
#include "bdev_lsvd.h"
#include "config.h"
#include "image.h"
#include "smartiov.h"
#include "utils.h"

FOLLY_GFLAGS_DECLARE_int64(lsvd_num_threads);

static int bdev_lsvd_init(void);
static void bdev_lsvd_finish(void);
static int bdev_lsvd_io_ctx_size(void);

enum class lsvd_iotype {
    READ = 1,
    WRITE = 2,
    FLUSH = 3,
    TRIM = 4,
};

auto format_as(lsvd_iotype t) { return fmt::underlying(t); }

struct lsvd_bdev_io {
    spdk_thread *submit_td;
    spdk_bdev_io_status status;
    lsvd_iotype type;
    io_timing tim;
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
                // set affinity to only the cpus in the current numa domain
                rte_cpuset_t cpuset;
                CPU_ZERO(&cpuset);

                auto cur_node = numa_node_of_cpu(sched_getcpu());
                for (int i = 0; i < CPU_SETSIZE; i++)
                    if (numa_node_of_cpu(i) == cur_node)
                        CPU_SET(i, &cpuset);

                rte_thread_set_affinity(&cpuset);
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
    lsvd_tp_inst([]() {
        auto tf = LsvdThreadFactory::getInstance();
        return new folly::CPUThreadPoolExecutor(FLAGS_lsvd_num_threads, tf);
    });

auto get_exe() { return folly::getKeepAliveToken(*lsvd_tp_inst.try_get()); }

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
        bdev.blocklen = 512;
        bdev.blockcnt = img->get_size() / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &lsvd_if;
        bdev.max_rw_size = 128 * 1024;
        bdev.fn_table = &lsvd_fn_table;

        kexe = get_exe();
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

auto noop_fn() -> TaskUnit { co_return folly::Unit(); }
void report_io_timing(lsvd_iotype type, io_timing &tim);

static void lsvd_submit_io(spdk_io_channel *c, spdk_bdev_io *io)
{
    auto dev = static_cast<lsvd_iodevice *>(io->bdev->ctxt);
    auto exe = dev->kexe;
    auto &img = dev->img;
    auto lio = (lsvd_bdev_io *)(io->driver_ctx);
    lio->submit_td = spdk_io_channel_get_thread(c);

    // io details
    auto offset = io->u.bdev.offset_blocks * io->bdev->blocklen;
    auto len = io->u.bdev.num_blocks * io->bdev->blocklen;

    lio->tim.submit = tnow();

    auto comp = [lio](auto &&ret) {
        auto sth = lio->submit_td;
        assert(sth != nullptr);

        if (ret.hasValue() && ret->ok()) [[likely]]
            lio->status = SPDK_BDEV_IO_STATUS_SUCCESS;
        else {
            lio->status = SPDK_BDEV_IO_STATUS_FAILED;
            if (ret.hasException())
                XLOGF(ERR, "IO failed with exception: {}",
                      ret.exception().what());
            else
                XLOGF(ERR, "IO failed with error: {}",
                      ret->status().ToString());
        }

        lio->tim.done = tnow();

        spdk_thread_send_msg(
            sth,
            [](void *ctx) {
                auto io = static_cast<decltype(lio)>(ctx);
                io->tim.complete = tnow();
                spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), io->status);
                report_io_timing(io->type, io->tim);
            },
            lio);
    };

    if (LSVD_IS_NOOP) {
        noop_fn().scheduleOn(exe).start(comp);
        return;
    }

    switch (io->type) {
    case SPDK_BDEV_IO_TYPE_READ: {
        lio->type = lsvd_iotype::READ;
        auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
        img->read(offset, iov, lio->tim).scheduleOn(exe).start(comp);
        break;
    }
    case SPDK_BDEV_IO_TYPE_WRITE: {
        lio->type = lsvd_iotype::WRITE;
        auto iov = smartiov::from_iovecs(io->u.bdev.iovs, io->u.bdev.iovcnt);
        img->write(offset, iov, lio->tim).scheduleOn(exe).start(comp);
        break;
    }
    case SPDK_BDEV_IO_TYPE_UNMAP:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        lio->type = lsvd_iotype::TRIM;
        img->trim(offset, len, lio->tim).scheduleOn(exe).start(comp);
        break;
    case SPDK_BDEV_IO_TYPE_FLUSH:
        lio->type = lsvd_iotype::FLUSH;
        img->flush(lio->tim).scheduleOn(exe).start(comp);
        break;
    default:
        XLOGF(ERR, "Unknown request type: {}", io->type);
        return;
    }
}

// Just copying from bdev_rbd, not sure where this is actually used
struct lsvd_bdev_io_channel {
    lsvd_iodevice *lsvd_dev;
    spdk_io_channel *io_channel;
};

auto bdev_lsvd_create(str pool_name, str img_name,
                      str cfg) -> Result<folly::Unit>
{
    XLOGF(INFO, "Creating LSVD device '{}'", img_name);
    assert(!img_name.empty());

    auto create_lsvd = [&]() -> void * {
        auto s3 = ObjStore::connect_to_pool(pool_name);
        if (!s3.ok()) {
            XLOGF(ERR, "Failed to connect to pool '{}'", pool_name);
            return nullptr;
        }

        auto img = LsvdImage::mount(s3.value(), img_name, cfg)
                       .scheduleOn(get_exe())
                       .start()
                       .wait()
                       .value();
        if (!img.ok()) {
            XLOGF(ERR, "Failed to mount image '{}': {}", img_name,
                  img.status().ToString());
            return nullptr;
        }

        auto iodev = new lsvd_iodevice(std::move(img.value()));
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
    auto name = iodev->img->name;
    XLOGF(INFO, "Destroying LSVD bdev {}", name);
    std::ignore = iodev->img->unmount().scheduleOn(iodev->kexe).start().wait();
    delete iodev;
    XLOGF(DBG1, "Destroyed LSVD bdev {}", name);
    return 0;
}

void report_io_timing(lsvd_iotype type, io_timing &tim)
{
    if (!fLB::FLAGS_lsvd_report_iotiming)
        return;

    static std::atomic<u64> total = 0;
    auto lat = tdiff_ns(tim.submit, tim.complete);

    if (!(total.fetch_add(1) % 20'000 == 1 ||
          (type == lsvd_iotype::WRITE && lat > LONG_WRITE_NS_THRES) ||
          (type == lsvd_iotype::READ && lat > LONG_READ_NS_THRES)))
        return;

    // clang-format off
    XLOGF(DBG6, "Op {}: {}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", 
        type, lat / 1000,
        tdiff_us(tim.submit, tim.start),
        tdiff_us(tim.start, tim.t1),
        tdiff_us(tim.t1, tim.t2),
        tdiff_us(tim.t3, tim.t2),
        tdiff_us(tim.t3, tim.t4),
        tdiff_us(tim.t5, tim.t4),
        tdiff_us(tim.t5, tim.t6),
        tdiff_us(tim.t7, tim.t6),
        tdiff_us(tim.t7, tim.t8),
        tdiff_us(tim.done, tim.t8),
        tdiff_us(tim.done, tim.complete)
        );
    // clang-format on
}
