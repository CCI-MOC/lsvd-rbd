#include "spdk/event.h"
#include <csignal>
#include <iostream>

#include "bdev_lsvd.h"
#include "utils.h"

struct start_lsvd_args {
    const char *pool_name;
    const char *image_name;
};

static void start_lsvd(void *arg)
{
    log_info("Starting LSVD SPDK program ...");

    setenv("LSVD_RCACHE_DIR", "/tmp/lsvd-read", 1);
    setenv("LSVD_WCACHE_DIR", "/tmp/lsvd-write", 1);
    setenv("LSVD_CACHE_SIZE", "2147483648", 1);

    auto args = (start_lsvd_args *)arg;

    rados_t cluster;
    int err = rados_create2(&cluster, "ceph", "client.admin", 0);
    check_ret_neg(err, "Failed to create cluster handle");

    err = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    check_ret_neg(err, "Failed to read config file");

    err = rados_connect(cluster);
    check_ret_neg(err, "Failed to connect to cluster");

    rados_ioctx_t io_ctx;
    err = rados_ioctx_create(cluster, args->pool_name, &io_ctx);
    check_ret_neg(err, "Failed to connect to pool {}", args->pool_name);

    err = bdev_lsvd_create(args->image_name, io_ctx);
    if (err) {
        log_error("Failed to create bdev");
        spdk_app_stop(err);
    }
}

int main(int argc, const char **argv)
{
    std::set_terminate([]() {
        try {
            std::cerr << boost::stacktrace::stacktrace();
        } catch (...) {
        }
        std::abort();
    });

    if (argc < 3) {
        log_error("Usage: {} <pool> <image>", argv[0]);
        return 1;
    }

    auto args = (start_lsvd_args){
        .pool_name = argv[1],
        .image_name = argv[2],
    };
    log_info("Args: pool={}, image={}", args.pool_name, args.image_name);

    std::signal(SIGINT, [](int) {
        log_info("Received SIGINT, shutting down LSVD SPDK program ...");
        spdk_app_stop(0);
    });

    spdk_app_opts opts = {.shutdown_cb = []() {
        log_info("Shutting down LSVD SPDK program ...");
        spdk_app_stop(0);
    }};

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spdk_frontend";

    int rc = spdk_app_start(&opts, start_lsvd, &args);
    spdk_app_fini();
    return rc;
}
