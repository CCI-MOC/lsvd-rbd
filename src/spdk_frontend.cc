#include "spdk/env.h"
#include "spdk/event.h"

#include "utils.h"

static void start_lsvd()
{
    log_info("Starting LSVD SPDK program ...");
}

int main(int argc, char **argv)
{
    spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spdk_frontend";

    int rc = spdk_app_start(&opts, NULL, NULL);
    spdk_app_fini();
    return rc;
}
