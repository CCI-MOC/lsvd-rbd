#include "folly/executors/GlobalExecutor.h"
#include "spdk/event.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "bdev_lsvd.h"
#include "image.h"
#include "representation.h"
#include "utils.h"

FOLLY_INIT_LOGGING_CONFIG(".=WARN,src=DBG7");

const char *NVME_SS_NQN = "nqn.2019-05.io.lsvd:cnode1";
const char *HOSTNAME = "127.0.0.1";
const char *PORT = "4420";

spdk_nvme_transport_id get_trid(const char *host, const char *port)
{
    spdk_nvme_transport_id trid;
    // They're fixed-size char[] bufs in the struct, so make sure we have space
    assert(strlen(host) < sizeof(trid.traddr));
    assert(strlen(port) < sizeof(trid.trsvcid));
    std::copy(host, host + strlen(host) + 1, trid.traddr);
    std::copy(port, port + strlen(port) + 1, trid.trsvcid);
    trid.trtype = SPDK_NVME_TRANSPORT_TCP;
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    // This is required because spdk looks at trstring, not the trtype
    spdk_nvme_transport_id_populate_trstring(
        &trid, spdk_nvme_transport_id_trtype_str(trid.trtype));
    return trid;
}

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

spdk_nvmf_tgt *create_target()
{
    XLOGF(DBG7, "Creating NVMF target");
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
    XLOGF(DBG7, "Creating NVMF discovery subsystem");
    auto ss = spdk_nvmf_subsystem_create(
        tgt, SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT, 0);
    assert(ss != nullptr);
    spdk_nvmf_subsystem_set_allow_any_host(ss, true);
    return ss;
}

spdk_nvmf_subsystem *add_nvme_ss(spdk_nvmf_tgt *tgt)
{
    XLOGF(DBG7, "Creating SPDK controller subsystem");
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
    XLOGF(DBG7, "Creating TCP transport");
    spdk_nvmf_transport_opts opts;
    auto succ = spdk_nvmf_transport_opts_init("TCP", &opts, sizeof(opts));
    assert(succ == true);
    opts.io_unit_size = 131072;
    opts.max_qpairs_per_ctrlr = 8;
    opts.in_capsule_data_size = 8192;
    XLOGF(DBG7,
          "TCP transport opts: io_unit_size={}, max_qpairs_per_ctrlr={}, "
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

void add_tgt_transport(spdk_nvmf_tgt *tgt, spdk_nvmf_transport *tr,
                       std::function<void(int)> *cb)
{
    XLOGF(DBG7, "Adding transport to target");
    spdk_nvmf_tgt_add_transport(tgt, tr, invoke_and_free_cb, cb);
}

void start_tgt_listen(spdk_nvmf_tgt *tgt, spdk_nvme_transport_id trid)
{
    spdk_nvmf_listen_opts lopts;
    spdk_nvmf_listen_opts_init(&lopts, sizeof(lopts));
    auto rc = spdk_nvmf_tgt_listen_ext(tgt, &trid, &lopts);
    assert(rc == 0);
}

void add_ss_listener(spdk_nvmf_tgt *tgt, spdk_nvmf_subsystem *ss,
                     spdk_nvme_transport_id trid, std::function<void(int)> *cb)
{
    XLOGF(DBG7, "Adding listener to subsystem");

    spdk_nvmf_listener_opts lopts;
    spdk_nvmf_subsystem_listener_opts_init(&lopts, sizeof(lopts));
    lopts.secure_channel = false;
    spdk_nvmf_subsystem_add_listener_ext(ss, &trid, invoke_and_free_cb, cb,
                                         &lopts);
}

void add_bdev_ns(spdk_nvmf_subsystem *ss, str bdev_name)
{
    XLOGF(DBG7, "Adding bdev namespace to subsystem");
    spdk_nvmf_ns_opts nopts;
    spdk_nvmf_ns_opts_get_defaults(&nopts, sizeof(nopts));
    auto nsid = spdk_nvmf_subsystem_add_ns_ext(ss, bdev_name.c_str(), &nopts,
                                               sizeof(nopts), nullptr);
    assert(nsid != 0);
}

void start_ss(spdk_nvmf_subsystem *ss, std::function<void(int)> *cb)
{
    // XLOGF(DBG7, "Starting subsystem");
    spdk_nvmf_subsystem_start(
        ss,
        [](auto ss, auto arg, auto rc) {
            auto cb = static_cast<std::function<void(int)> *>(arg);
            (*cb)(rc);
            delete cb;
        },
        cb);
}

static void setup_bdev_target(str bdev_name)
{
    XLOGF(INFO, "Configuring SPDK NVMe-oF target");

    // Setup spdk nvmf
    auto tgt = create_target();
    auto disc_ss = add_discovery_ss(tgt);
    auto nvme_ss = add_nvme_ss(tgt);
    auto trid = get_trid(HOSTNAME, PORT);

    add_bdev_ns(nvme_ss, bdev_name);

    // some stupid formatting decisions up ahead due to tower-of-callback
    // it also looks cleaner without indents
    // clang-format off
    create_tcp_transport(new TranspCb([=](auto *tr) { 
    assert(tr != nullptr);

    add_tgt_transport(tgt, tr, alloc_cb([=](int rc) {
    assert(rc == 0);

    start_tgt_listen(tgt, trid);
    add_ss_listener(tgt, disc_ss, trid, alloc_cb([=](int) { 
    add_ss_listener(tgt, nvme_ss, trid, alloc_cb([=](int rc) {
    assert(rc == 0);

    // Start both subsystems
    start_ss(nvme_ss, alloc_cb([=](int) {
    start_ss(disc_ss, alloc_cb([=](int) {

    XLOGF(INFO, "Successfully configured SPDK NVMe-oF target");

    })); })); })); })); })); }));
    // clang-format on
}

const usize GIB = 1024 * 1024 * 1024;

using StartFn = std::function<void(void)>;
static void call_fn(void *arg)
{
    auto fn = (StartFn *)arg;
    (*fn)();
}

int main(int argc, char **argv)
{
    fLU::FLAGS_folly_global_cpu_executor_threads =
        std::thread::hardware_concurrency();

    int fake_argc = 0;
    auto folly_init = folly::Init(&fake_argc, &argv, false);

    StartFn start_fn;
    str mode = argc > 1 ? argv[1] : "nomount";

    if (mode == "nomount") {
        if (argc > 2)
            XLOGF(FATAL, "Usage: {} nomount", argv[0]);

        start_fn = []() { XLOGF(INFO, "Starting SPDK app ..."); };
    }

    else if (mode == "mount_lsvd") {
        if (argc < 4)
            XLOGF(FATAL, "Usage: {} mount_lsvd [pool] [image_name]", argv[0]);

        auto pool_name = argv[2];
        auto image_name = argv[3];
        XLOGF(INFO, "Auto-mounting '{}'/'{}' on start", pool_name, image_name);

        start_fn = [=]() {
            auto res = bdev_lsvd_create(pool_name, image_name, "");
            assert(res.has_value());
            setup_bdev_target(image_name);
        };
    }

    else if (mode == "mount_noop") {
        if (argc < 4)
            XLOGF(FATAL, "Usage: {} mount_noop [name] [size]", argv[0]);

        auto name = argv[2];
        auto size = parse_size_str(argv[3]);
        XLOGF(INFO, "Auto-mounting noop image '{}'/{} on start", name, size);

        start_fn = [=]() {
            auto res = bdev_noop_create(name, size);
            assert(res.has_value());
            setup_bdev_target(name);
        };
    }

    else {
        XLOGF(FATAL, "Usage: {} [nomount|mount_noop|mount_lsvd]", argv[0]);
    }

    XLOGF(INFO, "Starting SPDK target, pid={}", getpid());

    ReadCache::init_cache(10 * GIB, 10 * GIB, "/tmp/lsvd.rcache");

    spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "lsvd_tgt";
    opts.reactor_mask = "[0,1,2,3]"; // 6 cores for reactor

    int rc = spdk_app_start(&opts, call_fn, &start_fn);
    spdk_app_fini();

    return rc;
}
