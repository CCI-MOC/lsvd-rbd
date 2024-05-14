#include "rados/librados.h"
#include "spdk/bdev_module.h"

#include "bdev_lsvd.h"
#include "image.h"
#include <future>

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

static bool lsvd_bdev_io_type_supported(void *ctx, spdk_bdev_io_type io_type);

static const spdk_bdev_fn_table lsvd_fn_table = {
    .destruct = nullptr,
    .submit_request = nullptr,
    .io_type_supported = &lsvd_bdev_io_type_supported,
    .get_io_channel = nullptr,
    .dump_info_json = nullptr,
};

static bool lsvd_bdev_io_type_supported(void *ctx, spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_FLUSH:        // we only use this to ensure ordering
    case SPDK_BDEV_IO_TYPE_RESET:        // block until all pending io aborts
    case SPDK_BDEV_IO_TYPE_UNMAP:        // trim
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES: // also just trim
        return true;
    default:
        return false;
    }
}

class lsvd_iodevice
{
  public:
    spdk_bdev bdev;
    uptr<lsvd_image> img;

    lsvd_iodevice(uptr<lsvd_image> img) : img(std::move(img))
    {
        bdev.product_name = (char *)"Log-structured Virtual Disk";
        bdev.name = (char *)img->image_name.c_str();
        bdev.blocklen = 4096;
        bdev.blockcnt = img->size / bdev.blocklen;
        bdev.ctxt = this;
        bdev.module = &lsvd_if;
        bdev.fn_table = &lsvd_fn_table;
    }

    ~lsvd_iodevice() {}
};

class lsvd_bdev_io_channel
{
};

int bdev_lsvd_create(std::string img_name, rados_ioctx_t ioctx)
{
    assert(!img_name.empty());

    auto img = lsvd_image::open_image(img_name, ioctx);
    auto iodev = new lsvd_iodevice(std::move(img));

    spdk_io_device_register(iodev, nullptr, nullptr,
                            sizeof(lsvd_bdev_io_channel), img_name.c_str());
    auto err = spdk_bdev_register(&iodev->bdev);
    if (err) {
        log_error("Failed to register bdev: err {}", (err));
        spdk_io_device_unregister(iodev, nullptr);
        return err;
    }

    return 0;
}

static void bdev_lsvd_delete_cb(void *arg, int rc)
{
    auto p = (std::promise<int> *)arg;
    p->set_value(rc);
}

int bdev_lsvd_delete(std::string img_name, std::function<void(int)> *cb)
{
    auto p = std::promise<int>();
    spdk_bdev_unregister_by_name(img_name.c_str(), &lsvd_if,
                                 bdev_lsvd_delete_cb, &p);
    return p.get_future().get();
}
