#include "spdk/event.h"
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "backend.h"
#include "image.h"
#include "representation.h"
#include "utils.h"

FOLLY_INIT_LOGGING_CONFIG(".=DBG,folly=INFO");

static void lsvd_tgt_usage() {}
static int lsvd_tgt_parse_arg(int ch, char *arg) { return 0; }

static void lsvd_tgt_started(void *arg1)
{
    XLOGF(INFO, "LSVD SPDK nvmf target started");
}

const usize GIB = 1024 * 1024 * 1024;

int main(int argc, char **argv)
{
    auto folly_init = folly::Init(&argc, &argv);
    ReadCache::init_cache(4 * GIB, 4 * GIB, "/tmp/lsvd.rcache");

    spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "lsvd_tgt";
    int rc;
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
                                  lsvd_tgt_parse_arg, lsvd_tgt_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    rc = spdk_app_start(&opts, lsvd_tgt_started, NULL);
    spdk_app_fini();

    // auto sf = main_task().scheduleOn(folly::getGlobalCPUExecutor()).start();
    // sf.wait();
    // auto res = sf.result();
    // if (res.hasException())
    //     XLOGF(ERR, "Error:\n{}", res.exception().what());
    // else if (res->has_error())
    //     XLOGF(ERR, "Error:\n{}", res->error().message());
    // else
    //     XLOGF(INFO, "Success:\n{}", res->value());
    return 0;
}