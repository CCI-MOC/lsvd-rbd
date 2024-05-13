#include "spdk/rpc.h"
#include "spdk/util.h"

#include "utils.h"

// RPC Command: bdev_lsvd_create <pool_name> <name>

struct rpc_bdev_lsvd_create {
    char *pool_name;
    char *name;
};

static const spdk_json_object_decoder rpc_create_lsvd_decoder[] = {
    {"pool_name", offsetof(rpc_bdev_lsvd_create, pool_name),
     spdk_json_decode_string, false},
    {"name", offsetof(rpc_bdev_lsvd_create, name), spdk_json_decode_string,
     false},
};

CEXTERN void rpc_bdev_lsvd_create(spdk_jsonrpc_request *request,
                                  const spdk_json_val *params)
{
    struct rpc_bdev_lsvd_create req = {};
    auto ret =
        spdk_json_decode_object(params, rpc_create_lsvd_decoder,
                                SPDK_COUNTOF(rpc_create_lsvd_decoder), &req);
}

SPDK_RPC_REGISTER("bdev_lsvd_create", rpc_bdev_lsvd_create, SPDK_RPC_RUNTIME);

// RPC Command: bdev_lsvd_delete <name>

struct rpc_bdev_lsvd_delete {
    char *name;
};

static const spdk_json_object_decoder rpc_delete_lsvd_decoder[] = {
    {"name", offsetof(rpc_bdev_lsvd_delete, name), spdk_json_decode_string,
     false},
};

CEXTERN void rpc_bdev_lsvd_delete(spdk_jsonrpc_request *request,
                                  const spdk_json_val *params)
{
    struct rpc_bdev_lsvd_delete req = {};
    auto ret =
        spdk_json_decode_object(params, rpc_delete_lsvd_decoder,
                                SPDK_COUNTOF(rpc_delete_lsvd_decoder), &req);
}

SPDK_RPC_REGISTER("bdev_lsvd_delete", rpc_bdev_lsvd_delete, SPDK_RPC_RUNTIME);
