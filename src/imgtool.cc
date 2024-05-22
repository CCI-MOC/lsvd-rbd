#include <argp.h>
#include <cstdlib>
#include <fcntl.h>
#include <fmt/format.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <uuid/uuid.h>

#include "backend.h"
#include "config.h"
#include "fake_rbd.h"
#include "lsvd_types.h"
#include "objects.h"
#include "translate.h"
#include "utils.h"

enum tool_operation { OP_CREATE = 1, OP_DELETE = 2, OP_INFO = 3, OP_CLONE = 5 };

const char *backend = "rados";
const char *image_name;
cfg_cache_type cache_type = LSVD_CFG_READ;
enum tool_operation op;
const char *pool_name = "lsvd";
size_t size = 0;

static long parseint(const char *_s)
{
    char *s = (char *)_s;
    long val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024 * 1024 * 1024);
    if (toupper(*s) == 'M')
        val *= (1024 * 1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

static struct argp_option options[] = {
    {"create", 'C', 0, 0, "create image", 0},
    {"size", 'z', "SIZE", 0, "size in bytes (M/G=2^20,2^30)", 0},
    {"delete", 'D', 0, 0, "delete image", 0},
    {"info", 'I', 0, 0, "show image information", 0},
    {"clone", 'c', "IMAGE", 0, "clone image", 0},
    {"pool", 'p', "POOL", 0, "pool name", 0},
    {0, 0, 0, 0, 0, 0},
};

static char args_doc[] = "IMAGE";

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case ARGP_KEY_ARG:
        image_name = arg;
        break;
    case 'C':
        op = OP_CREATE;
        break;
    case 'z':
        size = parseint(arg);
        break;
    case 'D':
        op = OP_DELETE;
        break;
    case 'I':
        op = OP_INFO;
        break;
    case 'c':
        op = OP_CLONE;
        break;
    case 'p':
        pool_name = arg;
    case ARGP_KEY_END:
        if (op == 0 || (op == OP_CREATE && size == 0))
            argp_usage(state);
        break;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, NULL, args_doc, 0, 0, 0};

void info(rados_ioctx_t io, const char *image_name)
{
    lsvd_config cfg;
    int rv;
    if ((rv = cfg.read()) < 0) {
        printf("error reading config: %d\n", rv);
        exit(1);
    }
    auto objstore = make_rados_backend(io);
    uuid_t uu;
    if ((rv = translate_get_uuid(objstore, image_name, uu)) < 0) {
        printf("error reading superblock: %d\n", rv);
        exit(1);
    }
    auto rcache_file = cfg.cache_filename(uu, image_name, LSVD_CFG_READ);
    auto wcache_file = cfg.cache_filename(uu, image_name, LSVD_CFG_WRITE);
    printf("image: %s\n", image_name);
    printf("read cache: %s\n", rcache_file.c_str());
    printf("write cache: %s\n", wcache_file.c_str());

    char base_buf[4096];
    rv = objstore->read(image_name, 0, base_buf, sizeof(base_buf));
    if (rv < 0)
        throw std::runtime_error("failed to read superblock");

    auto base_hdr = (common_obj_hdr *)base_buf;
    auto base_super = (super_hdr *)(base_hdr + 1);

    if (base_hdr->magic != LSVD_MAGIC || base_hdr->type != OBJ_SUPERBLOCK)
        throw std::runtime_error("corrupt superblock");

    char uuid_str[64];
    uuid_unparse_lower(base_hdr->vol_uuid, uuid_str);
    fmt::print("UUID: {}\n", uuid_str);
    fmt::print("Size: {} bytes", base_super->vol_size * 512);
    fmt::print(" / {} GiB\n",
               (double)base_super->vol_size * 512. / 1024. / 1024. / 1024.);
    fmt::print("Checkpoints: {}\n", base_super->ckpts_len / 4.);
    fmt::print("Snapshots: {}\n", base_super->snaps_len / 4.);
    fmt::print("Is a clone: {}\n", base_super->clones_len == 0 ? "no" : "yes");

    // parse clones
    if (base_super->clones_len == 0)
        return;

    uint32_t consumed = 0;
    while (consumed < base_super->clones_len) {
        auto ci =
            (clone_info *)(base_buf + base_super->clones_offset + consumed);
        auto objname = (char *)(ci + 1);
        auto upto_seq = ci->last_seq;
        fmt::print("Base: {}, upto seq {}\n", objname, upto_seq);
        consumed += sizeof(clone_info) + strlen(objname) + 1;
    }
}

int main(int argc, char **argv)
{
    std::set_terminate([]() {
        try {
            std::cerr << boost::stacktrace::stacktrace();
        } catch (...) {
        }
        std::abort();
    });

    argp_parse(&argp, argc, argv, 0, 0, 0);

    rados_t cluster;
    int err = rados_create2(&cluster, "ceph", "client.admin", 0);
    check_ret_neg(err, "Failed to create cluster handle");

    err = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    check_ret_neg(err, "Failed to read config file");

    err = rados_connect(cluster);
    check_ret_neg(err, "Failed to connect to cluster");

    rados_ioctx_t io_ctx;
    err = rados_ioctx_create(cluster, pool_name, &io_ctx);
    check_ret_neg(err, "Failed to connect to pool {}", pool_name);

    if (op == OP_CREATE && size > 0)
        rbd_create(io_ctx, image_name, size, NULL);
    else if (op == OP_DELETE)
        rbd_remove(io_ctx, image_name);
    else if (op == OP_INFO)
        info(io_ctx, image_name);
    else if (op == OP_CLONE) {
        auto src_img = image_name;
        auto dst_img = argv[argc - 1];
        fmt::print("cloning from {} to {}\n", src_img, dst_img);
        rbd_clone(io_ctx, src_img, dst_img);
    }

    rados_ioctx_destroy(io_ctx);
    rados_shutdown(cluster);
}
