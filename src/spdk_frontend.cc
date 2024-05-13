#include "spdk/env.h"
#include "spdk/event.h"

int main(int argc, char **argv)
{
    spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "spdk_frontend";
    opts.core_mask = "0x1";
    opts.shm_id = 0;
    spdk_env_init(&opts);

    spdk_env_thread_wait_all();

    return 0;
}
