#include <bdus.h>
#include <stdio.h>

extern int c_read(char*, uint64_t, uint32_t, struct bdus_ctx*);
extern int c_write(const char*, uint64_t, uint32_t, struct bdus_ctx*);
extern void c_init(int n);
extern int c_flush(struct bdus_ctx* ctx);
extern void c_shutdown(void);

int do_init(struct bdus_ctx* ctx)
{
    c_init(1);
    return 0;
}

int do_terminate(struct bdus_ctx* ctx)
{
    c_shutdown();
    return 0;
}

int do_flush(struct bdus_ctx *ctx)
{
    c_flush(ctx);
    return 0;
}

static const struct bdus_ops device_ops =
{
    .read       = c_read,
    .write      = c_write,
    .initialize = do_init,
    .flush      = do_flush,
    .terminate  = do_terminate,
};

static const struct bdus_attrs device_attrs =
{
    .size               = 1 << 30, // 1 GiB
    .logical_block_size = 512,
    .dont_daemonize     = 1,
};

int main(void)
{
    c_init(1);
    bool success = bdus_run(&device_ops, &device_attrs, NULL);
    if (!success)
        fprintf(stderr, "Error: %s\n", bdus_get_error_message());
    return success ? 0 : 1;
}
