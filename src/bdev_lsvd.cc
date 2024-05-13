#include "rados/librados.h"
#include "spdk/bdev_module.h"

#include "bdev_lsvd.h"

static int bdev_lsvd_init(void);
static void bdev_lsvd_finish(void);

static spdk_bdev_module lsvd_if = {
    .module_init = bdev_lsvd_init,
    .module_fini = bdev_lsvd_finish,
    .name = "lsvd",
};
SPDK_BDEV_MODULE_REGISTER(ext_lsvd, &lsvd_if);

static int bdev_lsvd_init(void) { return 0; }
static void bdev_lsvd_finish(void) {}

static const spdk_bdev_fn_table lsvd_fn_table = {
    .destruct = nullptr,
    .submit_request = nullptr,
    .io_type_supported = nullptr,
    .get_io_channel = nullptr,
    .dump_info_json = nullptr,
};

static int lsvd_bdev_io_type_supported(void *ctx, spdk_bdev_io_type io_type)
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

CEXTERN int bdev_lsvd_create(const char *pool_name, const char *img_name)
{
    assert(pool_name != nullptr);
    assert(img_name != nullptr);

    rados_t cluster;
    int err = rados_create2(&cluster, "ceph", "client.admin", 0);
    check_ret_neg(err, "Failed to create cluster handle");

    err = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    check_ret_neg(err, "Failed to read config file");

    err = rados_connect(cluster);
    check_ret_neg(err, "Failed to connect to cluster");

    rados_ioctx_t io_ctx;
    err = rados_ioctx_create(cluster, pool_name, &io_ctx);
    check_ret_neg(err, "Failed to connect to pool {}", pool_name);

    UNIMPLEMENTED();
    return 0;
}

CEXTERN int bdev_lsvd_delete(const char *img_name)
{
    UNIMPLEMENTED();
    return 0;
}
