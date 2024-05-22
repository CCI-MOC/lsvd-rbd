#include "spdk/event.h"
#include <csignal>
#include <iostream>

#include "bdev_lsvd.h"
#include "utils.h"

static void start_lsvd(void *arg)
{
    log_info("Starting LSVD SPDK program ...");

    setenv("LSVD_RCACHE_DIR", "/tmp/lsvd-read", 1);
    setenv("LSVD_WCACHE_DIR", "/tmp/lsvd-write", 1);
    setenv("LSVD_CACHE_SIZE", "2147483648", 1);

    std::string pool_name = "pone";

    rados_t cluster;
    int err = rados_create2(&cluster, "ceph", "client.admin", 0);
    check_ret_neg(err, "Failed to create cluster handle");

    err = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    check_ret_neg(err, "Failed to read config file");

    err = rados_connect(cluster);
    check_ret_neg(err, "Failed to connect to cluster");

    rados_ioctx_t io_ctx;
    err = rados_ioctx_create(cluster, pool_name.c_str(), &io_ctx);
    check_ret_neg(err, "Failed to connect to pool {}", pool_name);

    err = bdev_lsvd_create("test", io_ctx);

    spdk_app_stop(err);
}

int main(int argc, char **argv)
{
    std::set_terminate([]() {
        try {
            std::cerr << boost::stacktrace::stacktrace();
        } catch (...) {
        }
        std::abort();
    });

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

    int rc = spdk_app_start(&opts, start_lsvd, NULL);
    spdk_app_fini();
    return rc;
}
