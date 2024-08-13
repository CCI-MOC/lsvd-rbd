#include "spdk/event.h"

#include "utils.h"

static void lsvd_tgt_usage() {}
static int lsvd_tgt_parse_arg(int ch, char *arg) { return 0; }

static void lsvd_tgt_started(void *arg1)
{
    log_info("LSVD SPDK nvmf target started");
}

int main(int argc, char **argv)
{
    int rc;
    struct spdk_app_opts opts = {};

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "lsvd_tgt";
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
                                  lsvd_tgt_parse_arg, lsvd_tgt_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    rc = spdk_app_start(&opts, lsvd_tgt_started, NULL);
    spdk_app_fini();
    return rc;
}
