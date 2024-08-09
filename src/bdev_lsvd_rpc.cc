#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "bdev_lsvd.h"
#include "utils.h"

/**
 * We only expose 2 RPC endpoints: create and delete. Unlike RBD, we will not
 * have commands to manage ceph clusters; each image will create its own.
 */

struct rpc_create_lsvd {
    char *image_name;
    char *pool_name;
    char *config;
};

static const struct spdk_json_object_decoder rpc_create_lsvd_decoders[] = {
    {"image_name", offsetof(rpc_create_lsvd, image_name),
     spdk_json_decode_string, false},
    {"pool_name", offsetof(rpc_create_lsvd, pool_name), spdk_json_decode_string,
     false},
    {"config", offsetof(rpc_create_lsvd, config), spdk_json_decode_string,
     true},
};

static void rpc_bdev_lsvd_create(spdk_jsonrpc_request *req_json,
                                 const spdk_json_val *params)
{
    spdk_json_write_ctx *w;
    std::unique_ptr<rpc_create_lsvd, decltype([](auto p) {
                        free(p->image_name);
                        free(p->pool_name);
                        free(p->config);
                    })>
        req(new rpc_create_lsvd());

    auto rc = spdk_json_decode_object(params, rpc_create_lsvd_decoders,
                                      SPDK_COUNTOF(rpc_create_lsvd_decoders),
                                      req.get());
    PR_GOTO_IF(rc != 0, fail, "spdk_json_decode_object failed");

    rc = bdev_lsvd_create(req->pool_name, req->image_name, req->config);
    PR_GOTO_IF(rc != 0, fail, "failed to create lsvd bdev");

    w = spdk_jsonrpc_begin_result(req_json);
    PR_GOTO_IF(w == nullptr, fail, "failed to create json result");
    spdk_json_write_bool(w, true);
    spdk_jsonrpc_end_result(req_json, w);
    return;

fail:
    spdk_jsonrpc_send_error_response(req_json, rc,
                                     "failed to create lsvd bdev");
}

SPDK_RPC_REGISTER("bdev_lsvd_create", rpc_bdev_lsvd_create, SPDK_RPC_RUNTIME)

struct rpc_delete_lsvd {
    char *image_name;
};

static const struct spdk_json_object_decoder rpc_delete_lsvd_decoders[] = {
    {"image_name", offsetof(struct rpc_delete_lsvd, image_name),
     spdk_json_decode_string, false},
};

static void rpc_bdev_lsvd_delete(struct spdk_jsonrpc_request *req_json,
                                 const struct spdk_json_val *params)
{
    std::unique_ptr<rpc_delete_lsvd,
                    decltype([](auto p) { free(p->image_name); })>
        req(new rpc_delete_lsvd());

    int rc = spdk_json_decode_object(params, rpc_delete_lsvd_decoders,
                                     SPDK_COUNTOF(rpc_delete_lsvd_decoders),
                                     req.get());
    if (rc != 0) {
        spdk_jsonrpc_send_error_response(req_json, rc,
                                         "Failed to parse rpc json");
        return;
    }

    bdev_lsvd_delete(req->image_name, [=](int rc) {
        if (rc == 0) {
            auto w = spdk_jsonrpc_begin_result(req_json);
            spdk_json_write_bool(w, true);
            spdk_jsonrpc_end_result(req_json, w);
        } else {
            log_error("Failed to destroy lsvd bdev, rc = {}", rc);
            spdk_jsonrpc_send_error_response(req_json, rc,
                                             "Failed to destroy lsvd bdev");
        }
    });
}

SPDK_RPC_REGISTER("bdev_lsvd_delete", rpc_bdev_lsvd_delete, SPDK_RPC_RUNTIME)
