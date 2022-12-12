#include <stdio.h>
#include <stdlib.h>
#include <argp.h>

#include "fake_rbd.h"

const char *backend = "file";
const char *image_name;
const char *cache_dir;
enum _op {OP_CREATE = 1, OP_DELETE = 2};
enum _op op;
size_t size = 0;

static long parseint(const char *_s)
{
    char *s = (char*)_s;
    long val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024*1024*1024);
    if (toupper(*s) == 'M')
        val *= (1024*1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

static struct argp_option options[] = {
    {"cache-dir",'d', "DIR",  0, "cache directory"},
    {"rados",    'O', 0,      0, "use RADOS"},
    {"create",   'C', 0,      0, "create image"},
    {"size",     'z', "SIZE", 0, "size in bytes (M/G=2^20,2^30)"},
    {"delete",   'D', 0,      0, "delete image"},
    {0},
};

static char args_doc[] = "IMAGE";

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
    case ARGP_KEY_END:
	if (op == 0 || (op == OP_CREATE && size == 0))
	    argp_usage(state);
        break;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, NULL, args_doc};

int main(int argc, char **argv) {
    argp_parse (&argp, argc, argv, 0, 0, 0);

    setenv("LSVD_BACKEND", backend, 1);
    if (cache_dir != NULL)
	setenv("LSVD_CACHE_DIR", cache_dir, 1);
    rados_ioctx_t io = 0;
    if (op == OP_CREATE && size > 0)
	rbd_create(io, image_name, size, NULL);
    else if (op == OP_DELETE)
	rbd_remove(io, image_name);
}
