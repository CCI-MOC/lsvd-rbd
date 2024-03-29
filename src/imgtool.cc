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

enum tool_operation {
    OP_CREATE = 1,
    OP_DELETE = 2,
    OP_INFO = 3,
    OP_MKCACHE = 4,
    OP_CLONE = 5
};

const char *backend = "file";
const char *image_name;
const char *cache_dir;
const char *cache_dev;
cfg_cache_type cache_type = LSVD_CFG_READ;
enum tool_operation op;
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
    {"cache-dir", 'd', "DIR", 0, "cache directory", 0},
    {"rados", 'O', 0, 0, "use RADOS", 0},
    {"create", 'C', 0, 0, "create image", 0},
    {"mkcache", 'k', "DEV", 0, "use DEV as cache", 0},
    {"cache-type", 't', "R/W", 0,
     "R for read cache, W for write cache (default: R)", 0},
    {"size", 'z', "SIZE", 0, "size in bytes (M/G=2^20,2^30)", 0},
    {"delete", 'D', 0, 0, "delete image", 0},
    {"info", 'I', 0, 0, "show image information", 0},
    {"clone", 'c', "IMAGE", 0, "clone image", 0},
    {0, 0, 0, 0, 0, 0},
};

static char args_doc[] = "IMAGE";

extern int init_wcache(int fd, uuid_t &uuid, int n_pages);
int (*make_cache)(int fd, uuid_t &uuid, int n_pages) = init_wcache;

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case ARGP_KEY_ARG:
        image_name = arg;
        break;
    case 'd':
        cache_dir = arg;
        break;
    case 'O':
        backend = "rados";
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
    case 't':
        if (arg[0] == 'R') {
            cache_type = LSVD_CFG_READ;
            log_error("read cache no longer supported");
            exit(1);
        } else if (arg[0] == 'W') {
            cache_type = LSVD_CFG_WRITE;
            make_cache = init_wcache;
        } else
            argp_usage(state);
        break;
    case 'k':
        op = OP_MKCACHE;
        cache_dev = arg;
        break;
    case 'c':
        op = OP_CLONE;
        break;
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
    auto objstore = get_backend(&cfg, io, NULL);
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
    rv = objstore->read_object(image_name, base_buf, sizeof(base_buf), 0);
    if (rv < 0)
        throw std::runtime_error("failed to read superblock");

    auto base_hdr = (obj_hdr *)base_buf;
    auto base_super = (super_hdr *)(base_hdr + 1);

    if (base_hdr->magic != LSVD_MAGIC || base_hdr->type != LSVD_SUPER)
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

void mk_cache(rados_ioctx_t io, const char *image_name, const char *dev_name,
              cfg_cache_type type)
{
    int rv, fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        perror("device file open");
        exit(1);
    }
    auto sz = getsize64(fd);

    lsvd_config cfg;
    if ((rv = cfg.read()) < 0) {
        printf("error reading config: %d\n", rv);
        exit(1);
    }
    auto objstore = get_backend(&cfg, io, NULL);
    uuid_t uu;
    if ((rv = translate_get_uuid(objstore, image_name, uu)) < 0) {
        printf("error reading superblock: %d\n", rv);
        exit(1);
    }
    auto cache_file = cfg.cache_filename(uu, image_name, type);

    auto n_pages = sz / 4096;
    if (make_cache(fd, uu, n_pages) < 0) {
        printf("make_cache failed\n");
        exit(1);
    }
    if ((rv = symlink(dev_name, cache_file.c_str())) < 0) {
        perror("symbolic link");
        exit(1);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    argp_parse(&argp, argc, argv, 0, 0, 0);

    setenv("LSVD_BACKEND", backend, 1);
    if (cache_dir != NULL) {
        if (cache_type == LSVD_CFG_READ)
            setenv("LSVD_RCACHE_DIR", cache_dir, 1);
        else
            setenv("LSVD_WCACHE_DIR", cache_dir, 1);
    }
    rados_ioctx_t io = 0;
    if (op == OP_CREATE && size > 0)
        rbd_create(io, image_name, size, NULL);
    else if (op == OP_DELETE)
        rbd_remove(io, image_name);
    else if (op == OP_INFO)
        info(io, image_name);
    else if (op == OP_MKCACHE)
        mk_cache(io, image_name, cache_dev, cache_type);
    else if (op == OP_CLONE) {
        auto src_img = image_name;
        auto dst_img = argv[argc - 1];
        fmt::print("cloning from {} to {}\n", src_img, dst_img);
        rbd_clone(io, src_img, dst_img);
    }
}
