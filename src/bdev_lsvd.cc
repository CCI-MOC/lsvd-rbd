#include "folly/executors/GlobalExecutor.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"

#include "backend.h"
#include "bdev_lsvd.h"
#include "image.h"
#include "smartiov.h"
#include "utils.h"

static int bdev_lsvd_init(void);
static void bdev_lsvd_finish(void);
static int bdev_lsvd_io_ctx_size(void);

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

class lsvd_iodevice
{
  public:
    spdk_bdev bdev;
    uptr<LsvdImage> img;

    lsvd_iodevice(uptr<LsvdImage> img_) : img(std::move(img_))
    {
        std::memset(&bdev, 0, sizeof(bdev));
        bdev.product_name = strdup("Log-structured Virtual Disk");
        bdev.name = strdup(img->name.c_str());
        bdev.blocklen = 4096;
        bdev.blockcnt = img->get_size() / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &lsvd_if;
        // bdev.max_rw_size = 128 * 1024;
        bdev.fn_table = &lsvd_fn_table;
    }

    ~lsvd_iodevice()
    {
        free(bdev.product_name);
        free(bdev.name);
    }
};

static int lsvd_destroy_bdev(void *ctx)
{
    auto iodev = reinterpret_cast<lsvd_iodevice *>(ctx);
    delete iodev;
    return 0;
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

struct lsvd_bdev_io {
    spdk_thread *submit_td;
    spdk_bdev_io_status status;
};

static int bdev_lsvd_io_ctx_size(void) { return sizeof(lsvd_bdev_io); }

static void lsvd_io_done(lsvd_bdev_io *io, folly::Try<Result<void>> ret)
{
    auto sth = io->submit_td;
    assert(sth != nullptr);

    if (ret.hasValue() && ret->has_value())
        io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
    else
        io->status = SPDK_BDEV_IO_STATUS_FAILED;

    spdk_thread_send_msg(
        sth,
        [](void *ctx) {
            auto io = (lsvd_bdev_io *)ctx;
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), io->status);
        },
        io);
}

static void lsvd_submit_io(spdk_io_channel *c, spdk_bdev_io *io)
{
    auto dev = static_cast<lsvd_iodevice *>(io->bdev->ctxt);
    auto &img = dev->img;
    auto lio = (lsvd_bdev_io *)(io->driver_ctx);
    lio->submit_td = spdk_io_channel_get_thread(c);

    // io details
    auto offset = io->u.bdev.offset_blocks * io->bdev->blocklen;
    auto len = io->u.bdev.num_blocks * io->bdev->blocklen;
    smartiov iov(io->u.bdev.iovs, io->u.bdev.iovcnt);

    auto exe = folly::getGlobalCPUExecutor();
    auto cb = [lio](auto &&r) { lsvd_io_done(lio, r); };

    switch (io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        img->read(offset, iov).scheduleOn(exe).start().defer(cb);
        break;
    case SPDK_BDEV_IO_TYPE_WRITE:
        img->write(offset, iov).scheduleOn(exe).start().defer(cb);
        break;
    case SPDK_BDEV_IO_TYPE_FLUSH:
        img->flush().scheduleOn(exe).start().defer(cb);
        break;
    case SPDK_BDEV_IO_TYPE_UNMAP:
        img->trim(offset, len).scheduleOn(exe).start().defer(cb);
        break;
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        img->trim(offset, len).scheduleOn(exe).start().defer(cb);
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

auto bdev_lsvd_create(str pool_name, str img_name, str cfg) -> Result<void>
{
    auto s3 = ObjStore::connect_to_pool(pool_name);
    LOGERR_AND_RET_IF_FAIL(s3, "Failed to connect to pool '{}'", pool_name);
    assert(!img_name.empty());

    auto img = LsvdImage::mount(s3.value(), img_name, cfg)
                   .scheduleOn(folly::getGlobalCPUExecutor())
                   .start()
                   .wait()
                   .value();
    LOGERR_AND_RET_IF_FAIL(img, "Failed to mount image '{}'", img_name);
    auto iodev = new lsvd_iodevice(std::move(img.value()));

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

        return std::error_code(err, std::system_category());
    }

    return outcome::success();
}

void bdev_lsvd_delete(str img_name, std::function<void(Result<void>)> cb)
{
    XLOGF(INFO, "Deleting image '{}'", img_name);
    auto rc = spdk_bdev_unregister_by_name(
        img_name.c_str(), &lsvd_if,
        // some of the ugliest lifetime management code you'll ever see, but
        // it should work
        [](void *arg, int rc) {
            XLOGF(INFO, "Image deletion done, rc = {}", rc);
            auto cb = (std::function<void(Result<void>)> *)arg;
            (*cb)(errcode_to_result<void>(rc));
            delete cb;
        },
        new std::function<void(Result<void>)>(cb));

    if (rc != 0) {
        XLOGF(ERR, "Failed to delete image '{}': {}", img_name, rc);
        cb(outcome::failure(std::error_code(rc, std::system_category())));
    }
}
