#include <bdus.h>
#include <stdio.h>

//typedef void* rados_ioctx_t;
//typedef void* rbd_image_t;

#include "fake_rbd.h"

extern int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
                    const char *snap_name);
extern int rbd_close(rbd_image_t image);

extern int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf);
extern int rbd_write(rbd_image_t image, uint64_t off, size_t len, char *buf);
extern int rbd_flush(rbd_image_t image);

int do_init(struct bdus_ctx* ctx)
{
    return 0;
}

int do_terminate(struct bdus_ctx* ctx)
{
    rbd_image_t img = ctx->private_data;
    return rbd_close(img);
}

int do_read(char *buffer, uint64_t offset, uint32_t size, struct bdus_ctx *ctx)
{
    rbd_image_t img = ctx->private_data;
    return rbd_read(img, offset, size, buffer);
}

int do_write(const char *buffer, uint64_t offset, uint32_t size, struct bdus_ctx *ctx)
{
    rbd_image_t img = ctx->private_data;
    return rbd_write(img, offset, size, buffer);
}

int do_flush(struct bdus_ctx *ctx)
{
    rbd_image_t img = ctx->private_data;
    return rbd_flush(img);
}

static const struct bdus_ops device_ops =
{
    .read       = do_read,
    .write      = do_write,
    .initialize = do_init,
    .flush      = do_flush,
    .terminate  = do_terminate,
};

static struct bdus_attrs device_attrs =
{
    .size               = 1 << 30, // 1 GiB
    .logical_block_size = 512,
    .dont_daemonize     = 1,
//    .max_concurrent_callbacks = 16,
};

int main(int argc, char **argv)
{
    rbd_image_t img;
    int rv = rbd_open(NULL, argv[1], &img, NULL);
    if (rv < 0)
        fprintf(stderr, "failed to open\n"), exit(1);
    
    rbd_image_info_t info;
    rbd_stat(img, &info, sizeof(info));
    device_attrs.size = info.size;
    
    bool success = bdus_run(&device_ops, &device_attrs, img);
    if (!success)
        fprintf(stderr, "Error: %s\n", bdus_get_error_message());
    return success ? 0 : 1;
}
