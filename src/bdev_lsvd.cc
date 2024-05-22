#include "rados/librados.h"
#include "spdk/bdev_module.h"
#include <future>

#include "bdev_lsvd.h"
#include "image.h"
#include "smartiov.h"
#include "spdk/thread.h"
#include "utils.h"

static int bdev_lsvd_init(void);
static void bdev_lsvd_finish(void);

static spdk_bdev_module lsvd_if = {
    .module_init = bdev_lsvd_init,
    .module_fini = bdev_lsvd_finish,
    .name = "LSVD bdev module",
};
SPDK_BDEV_MODULE_REGISTER(ext_lsvd, &lsvd_if);

static int bdev_lsvd_init(void) { return 0; }
static void bdev_lsvd_finish(void) {}

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
    uptr<lsvd_image> img;

    lsvd_iodevice(uptr<lsvd_image> img_) : img(std::move(img_))
    {
        bdev.product_name = strdup("Log-structured Virtual Disk");
        bdev.name = strdup(img->imgname.c_str());
        bdev.blocklen = 4096;
        bdev.blockcnt = img->size / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &lsvd_if;
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
    return spdk_get_io_channel(iodev);
}

static void lsvd_submit_io(spdk_io_channel *c, spdk_bdev_io *io)
{
    auto dev = static_cast<lsvd_iodevice *>(io->bdev->ctxt);
    auto &img = dev->img;

    // io details
    auto offset = io->u.bdev.offset_blocks * io->bdev->blocklen;
    auto len = io->u.bdev.num_blocks * io->bdev->blocklen;
    smartiov iov(io->u.bdev.iovs, io->u.bdev.iovcnt);

    request *r;
    switch (io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        r = img->read(offset, iov, nullptr);
        break;
    case SPDK_BDEV_IO_TYPE_WRITE:
        r = img->write(offset, iov, nullptr);
        break;
    case SPDK_BDEV_IO_TYPE_FLUSH:
        r = img->flush(nullptr);
        break;
    case SPDK_BDEV_IO_TYPE_UNMAP:
        r = img->trim(offset, len, nullptr);
        break;
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        r = img->trim(offset, len, nullptr);
        break;
    default:
        log_error("Unknown request type: {}", io->type);
        return;
    }

    r->run(nullptr);
}

// Just copying from bdev_rbd, not sure where this is actually used
struct lsvd_bdev_io_channel {
    lsvd_iodevice *lsvd_dev;
    spdk_io_channel *io_channel;
};

int bdev_lsvd_create(std::string img_name, rados_ioctx_t ioctx)
{
    assert(!img_name.empty());

    lsvd_config cfg; // TODO
    uptr<lsvd_image> img;
    try {
        img = uptr<lsvd_image>(new lsvd_image(img_name, ioctx, cfg));
    } catch (std::runtime_error &e) {
        log_error("Failed to create image '{}': {}", img_name, e.what());
        return -1;
    }

    auto iodev = new lsvd_iodevice(std::move(img));

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
        log_error("Failed to register bdev: err {}", (err));
        spdk_io_device_unregister(
            iodev, [](void *ctx) { delete (lsvd_iodevice *)ctx; });
        return err;
    }

    return 0;
}

int bdev_lsvd_delete(std::string img_name)
{
    auto p = std::promise<int>();
    spdk_bdev_unregister_by_name(
        img_name.c_str(), &lsvd_if,
        [](void *arg, int rc) {
            auto p = (std::promise<int> *)arg;
            p->set_value(rc);
        },
        &p);
    return p.get_future().get();
}
