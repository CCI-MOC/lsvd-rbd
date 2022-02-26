#include <bdus.h>
#include <stdio.h>

extern int c_read(char*, uint64_t, uint32_t, struct bdus_ctx*);
extern int c_write(const char*, uint64_t, uint32_t, struct bdus_ctx*);

static const struct bdus_ops device_ops =
{
    .read  = c_read,
    .write = c_write,
};

static const struct bdus_attrs device_attrs =
{
    .size               = 1 << 30, // 1 GiB
    .logical_block_size = 512,
};

int main(void)
{
    bool success = bdus_run(&device_ops, &device_attrs, NULL);
    if (!success)
        fprintf(stderr, "Error: %s\n", bdus_get_error_message());
    return success ? 0 : 1;
}
