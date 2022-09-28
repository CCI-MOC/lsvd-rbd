#include <bdus.h>
#include <stdio.h>
#include <argp.h>

#include <sys/uio.h>
#include <rados/librados.h>
#include <stdlib.h>
#include <stdlib.h>

#include "fake_rbd.h"

extern int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
                    const char *snap_name);
extern int rbd_close(rbd_image_t image);

extern int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf);
extern int rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf);
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

/* including all the members in case we compile as C++
 */
static const struct bdus_ops device_ops =
{
    .initialize = do_init,
    .on_device_available = NULL,
    .terminate  = do_terminate,
    .read       = do_read,
    .write      = do_write,
    .write_same = NULL,
    .write_zeros = NULL,
    .fua_write = NULL,
    .flush      = do_flush,
    .discard = NULL,
    .secure_erase = NULL,
    .ioctl = NULL
};

static struct bdus_attrs device_attrs =
{
    .logical_block_size = 512,
    /* physical_block_size */
    .size               = 1 << 30, // 1 GiB
    .max_concurrent_callbacks = 0,
    /* max_read_write_size */
    /* max_write_same_size */
    /* max_write_zeros_size */
    /* max_discard_erase_size */
    /* disable_partition_scanning */
    /* recoverable */
    .dont_daemonize     = 1,
    /* log */
};


static char args_doc[] = "SSD OBJ_PREFIX";
static struct argp_option options[] = {
    {"threads",      't', "THREADS",   0, "max number of threads"},
    {0},
};
char *ssd, *prefix;

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 't':
        device_attrs.max_concurrent_callbacks = atoi(arg);
        break;
    case ARGP_KEY_ARG:
        if (ssd == NULL)
            ssd = arg;
        else if (prefix == NULL)
            prefix = arg;
        else
            return ARGP_ERR_UNKNOWN;
        break;
    case ARGP_KEY_END:
        if (!ssd || !prefix)
            argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, NULL, args_doc};

int main(int argc, char **argv)
{
    argp_parse (&argp, argc, argv, ARGP_LONG_ONLY, 0, NULL);
    char rbd_name[128];
    sprintf(rbd_name, "%s,%s", ssd, prefix);
    
    rbd_image_t img;
    int rv = rbd_open(NULL, rbd_name, &img, NULL);
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
