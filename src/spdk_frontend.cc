#include "spdk/event.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include <algorithm>
#include <csignal>
#include <future>
#include <iostream>

#include "backend.h"
#include "bdev_lsvd.h"
#include "spdk/nvmf_spec.h"
#include "utils.h"

const char *NVME_SS_NQN = "nqn.2019-05.io.lsvd:cnode1";
const char *HOSTNAME = "127.0.0.1";
const char *PORT = "4420";

using IntCallbackFn = std::function<void(int)>;
IntCallbackFn *alloc_cb(std::function<void(int)> cb)
{
    return new IntCallbackFn(cb);
}

void invoke_and_free_cb(void *ctx, int status)
{
    auto cb = static_cast<std::function<void(int)> *>(ctx);
    (*cb)(status);
    delete cb;
}

struct start_lsvd_args {
    const char *pool_name;
    const char *image_name;
};

spdk_nvmf_tgt *create_target()
{
    log_info("Creating NVMF target");
    spdk_nvmf_target_opts opts = {
        .name = "lsvd_nvmf_tgt",
        .discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY,
    };
    auto tgt = spdk_nvmf_tgt_create(&opts);
    assert(tgt != nullptr);

    auto pg = spdk_nvmf_poll_group_create(tgt);
    assert(pg != nullptr);

    return tgt;
}

spdk_nvmf_subsystem *add_discovery_ss(spdk_nvmf_tgt *tgt)
{
    log_info("Creating NVMF discovery subsystem");
    auto ss = spdk_nvmf_subsystem_create(
        tgt, SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT, 0);
    assert(ss != nullptr);
    spdk_nvmf_subsystem_set_allow_any_host(ss, true);
    return ss;
}

spdk_nvmf_subsystem *add_nvme_ss(spdk_nvmf_tgt *tgt)
{
    log_info("Creating SPDK controller subsystem");
    auto ss =
        spdk_nvmf_subsystem_create(tgt, NVME_SS_NQN, SPDK_NVMF_SUBTYPE_NVME, 1);
    assert(ss != nullptr);
    spdk_nvmf_subsystem_set_allow_any_host(ss, true);
    spdk_nvmf_subsystem_set_sn(ss, "SPDK_000001");
    spdk_nvmf_subsystem_set_mn(ss, "LSVD NVMe controller");
    spdk_nvmf_subsystem_set_ana_reporting(ss, true);
    return ss;
}

using TranspCb = std::function<void(spdk_nvmf_transport *)>;
void create_tcp_transport(TranspCb *cb)
{
    log_info("Creating TCP transport");
    spdk_nvmf_transport_opts opts;
    auto succ = spdk_nvmf_transport_opts_init("TCP", &opts, sizeof(opts));
    assert(succ == true);
    opts.io_unit_size = 131072;
    opts.max_qpairs_per_ctrlr = 8;
    opts.in_capsule_data_size = 8192;
    debug("TCP transport opts: io_unit_size={}, max_qpairs_per_ctrlr={}, "
          "in_capsule_data_size={}",
          opts.io_unit_size, opts.max_qpairs_per_ctrlr,
          opts.in_capsule_data_size);

    auto rc = spdk_nvmf_transport_create_async(
        "TCP", &opts,
        [](auto ctx, auto r) {
            auto cb = static_cast<TranspCb *>(ctx);
            (*cb)(r);
            delete cb;
        },
        cb);
    assert(rc == 0);
}

void add_transport(spdk_nvmf_tgt *tgt, spdk_nvmf_transport *tr,
                   std::function<void(int)> *cb)
{
    log_info("Adding transport to target");
    spdk_nvmf_tgt_add_transport(tgt, tr, invoke_and_free_cb, cb);
}

void add_ss_listener(spdk_nvmf_tgt *tgt, spdk_nvmf_subsystem *ss, str host,
                     str port, std::function<void(int)> *cb)
{
    log_info("Adding listener to subsystem");

    spdk_nvme_transport_id trid;
    // They're fixed-size char[] bufs in the struct, so make sure we have space
    assert(host.size() < sizeof(trid.traddr));
    assert(port.size() < sizeof(trid.trsvcid));
    std::copy(host.begin(), host.end(), trid.traddr);
    std::copy(port.begin(), port.end(), trid.trsvcid);
    trid.trtype = SPDK_NVME_TRANSPORT_TCP;
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    // This is required because spdk looks at trstring, not the trtype
    spdk_nvme_transport_id_populate_trstring(
        &trid, spdk_nvme_transport_id_trtype_str(trid.trtype));

    spdk_nvmf_listen_opts lopts1;
    spdk_nvmf_listen_opts_init(&lopts1, sizeof(lopts1));
    auto rc = spdk_nvmf_tgt_listen_ext(tgt, &trid, &lopts1);
    assert(rc == 0);

    spdk_nvmf_listener_opts lopts;
    spdk_nvmf_subsystem_listener_opts_init(&lopts, sizeof(lopts));
    lopts.secure_channel = false;

    spdk_nvmf_subsystem_add_listener_ext(ss, &trid, invoke_and_free_cb, cb,
                                         &lopts);
}

void add_bdev_ns(spdk_nvmf_subsystem *ss, str bdev_name)
{
    log_info("Adding bdev namespace to subsystem");
    spdk_nvmf_ns_opts nopts;
    spdk_nvmf_ns_opts_get_defaults(&nopts, sizeof(nopts));
    auto err = spdk_nvmf_subsystem_add_ns_ext(ss, bdev_name.c_str(), &nopts,
                                              sizeof(nopts), nullptr);
    assert(err == 0);
}

static void start_lsvd(void *arg)
{
    log_info("Starting LSVD SPDK program ...");
    auto args = (start_lsvd_args *)arg;

    auto io_ctx = connect_to_pool(args->pool_name);

    // Setup spdk nvmf
    auto tgt = create_target();
    auto disc_ss = add_discovery_ss(tgt);
    auto nvme_ss = add_nvme_ss(tgt);

    // Add lsvd bdev
    lsvd_config cfg;                    // TODO read this in from a config file
    cfg.cache_size = 160 * 1024 * 1024; // small 160mb cache for testing
    auto err = bdev_lsvd_create(args->image_name, io_ctx, cfg);
    assert(err == 0);
    add_bdev_ns(nvme_ss, args->image_name);

    // some stupid formatting decisions up ahead due to tower-of-callback
    // clang-format off
    create_tcp_transport(new TranspCb([=](auto *tr) { 
        assert(tr != nullptr);
        add_transport(tgt, tr, alloc_cb([=](int rc) {
            assert(rc == 0);
            add_ss_listener(tgt, nvme_ss, HOSTNAME, PORT, alloc_cb([=](int rc) {
                assert(rc == 0);
                // Start both subsystems
                spdk_nvmf_subsystem_start(nvme_ss, nullptr, nullptr);
                spdk_nvmf_subsystem_start(disc_ss, nullptr, nullptr);

                log_info("LSVD SPDK program started successfully");
            }));
        }));
    }));
    // clang-format on
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
