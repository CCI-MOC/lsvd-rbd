#include "spdk/event.h"
#include "spdk/nvmf.h"
#include <csignal>
#include <future>
#include <iostream>

#include "bdev_lsvd.h"
#include "spdk/nvmf_spec.h"
#include "utils.h"

struct start_lsvd_args {
    const char *pool_name;
    const char *image_name;
};

static void start_lsvd(void *arg)
{
    log_info("Starting LSVD SPDK program ...");
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

    lsvd_config cfg; // TODO get this from somewhere reasonable
    cfg.cache_size = 160 * 1024 * 1024;
    err = bdev_lsvd_create(args->image_name, io_ctx, cfg);
    if (err) {
        log_error("Failed to create bdev");
        spdk_app_stop(err);
    }

    // TODO setup nvmf subsystems and all that nonsense
    // we can worry about refactoring it into functions later

    // Step 1: create nvmf target
    log_info("Creating NVMF target");
    auto nvmf_opts = (spdk_nvmf_target_opts){
        .name = "lsvd_nvmf_tgt",
    };
    auto tgt = spdk_nvmf_tgt_create(&nvmf_opts);
    assert(tgt != nullptr);

    // Step 1.5: add discovery subsystem so we can probe for it
    log_info("Creating NVMF discovery subsystem");
    auto disc_ss = spdk_nvmf_subsystem_create(
        tgt, SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT, 0);
    assert(disc_ss != nullptr);
    spdk_nvmf_subsystem_set_allow_any_host(disc_ss, true);

    // Step 2: create TCP transport
    spdk_nvmf_transport_opts opts;
    auto succ = spdk_nvmf_transport_opts_init("TCP", &opts, sizeof(opts));
    assert(succ == true);
    // opts.io_unit_size = 131072;
    // opts.max_qpairs_per_ctrlr = 8;
    opts.in_capsule_data_size = 8192;
    debug("TCP transport opts: io_unit_size={}, max_qpairs_per_ctrlr={}, "
          "in_capsule_data_size={}",
          opts.io_unit_size, opts.max_qpairs_per_ctrlr,
          opts.in_capsule_data_size);

    log_info("Creating TCP transport");
    auto transport_p = std::promise<spdk_nvmf_transport *>();
    spdk_nvmf_transport_create_async(
        "TCP", &opts,
        [](auto p, auto b) {
            auto pr = (std::promise<spdk_nvmf_transport *> *)p;
            pr->set_value(b);
        },
        &transport_p);
    auto transport = transport_p.get_future().get();
    assert(transport != nullptr);

    log_info("Adding TCP transport to target");
    auto stat_p = std::promise<int>();
    spdk_nvmf_tgt_add_transport(
        tgt, transport,
        [](auto p, auto stat) {
            auto pr = (std::promise<int> *)p;
            pr->set_value(stat);
        },
        nullptr);
    auto status = stat_p.get_future().get();
    assert(status == 0);

    // Step 3: create subsystem for our bdev
    log_info("Creating SPDK controller");
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

    spdk_app_opts opts = {.shutdown_cb = []() {
        log_info("Shutting down LSVD SPDK program ...");
        spdk_app_stop(0);
    }};

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spdk_frontend";

    int rc = spdk_app_start(&opts, start_lsvd, &args);
    spdk_app_fini();

    log_info("Exiting ...");
    return rc;
}
