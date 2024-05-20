#include <algorithm>
#include <sys/uio.h>
#include <utility>
#include <zlib.h>

#include "lsvd_types.h"
#include "objects.h"
#include "utils.h"

void serialise_common_hdr(vec<byte> buf, obj_type t, seqnum_t s, u32 hdr,
                          u32 data, uuid_t &uuid)
{
    if (buf.size() < sizeof(common_obj_hdr))
        buf.resize(sizeof(common_obj_hdr));

    auto h = (common_obj_hdr *)buf.data();
    *h = (common_obj_hdr){.magic = LSVD_MAGIC,
                          .version = 1,
                          .vol_uuid = {0},
                          .type = t,
                          .seq = s,
                          .hdr_sectors = hdr,
                          .data_sectors = data,
                          .crc = 0};
    uuid_copy(h->vol_uuid, uuid);
}

void serialise_superblock(vec<byte> buf, vec<seqnum_t> &checkpoints,
                          vec<clone_base> &clones, uuid_t &uuid)
{
    usize req_size = sizeof(common_obj_hdr) + sizeof(super_hdr);
    req_size += checkpoints.size() * sizeof(seqnum_t);
    req_size = std::max(req_size, 8ul); // minimum of 4096 bytes
    req_size = round_up(req_size, 512); // round to sector boundary

    if (buf.size() < req_size)
        buf.resize(req_size);

    serialise_common_hdr(buf, OBJ_SUPERBLOCK, 0, req_size / 512, 0, uuid);

    auto h = (super_hdr *)(buf.data() + sizeof(common_obj_hdr));
    h->ckpts_offset = sizeof(common_obj_hdr) + sizeof(super_hdr);
    h->ckpts_len = checkpoints.size() * sizeof(seqnum_t);

    auto p = (seqnum_t *)(buf.data() + h->ckpts_offset);
    for (auto &c : checkpoints)
        *p++ = c;
}

opt<vec<byte>> object_reader::fetch_object_header(std::string objname)
{
    vec<byte> buf(4096);
    auto err = objstore->read(objname, 0, buf.data(), 4096);
    RET_IF(err != 4096, std::nullopt);

    auto h = (common_obj_hdr *)buf.data();

    // Validate magic
    PR_RET_IF(h->magic != LSVD_MAGIC || h->version != 1, std::nullopt,
              "Invalid magic or version in object '{}'", objname);

    if (h->hdr_sectors <= 8)
        return buf;

    // Header is longer than 4096, we have to fetch the rest
    auto len = h->hdr_sectors * 512;
    buf.reserve(len);
    err = objstore->read(objname, 0, buf.data(), len);
    PR_ERR_RET_IF(std::cmp_not_equal(err, len), std::nullopt, err,
                  "Failed to read object '{}' header", objname);

    return buf;
}

opt<parsed_superblock> object_reader::read_superblock(std::string oname)
{
    auto buf = objstore->read_whole_obj(oname);
    PASSTHRU_NULLOPT(buf);
    auto hdr = (common_obj_hdr *)buf->data();

    PR_RET_IF(hdr->magic != LSVD_MAGIC, std::nullopt,
              "Corrupt object; invalid magic at '{}'", oname);
    PR_RET_IF(hdr->version != 1, std::nullopt,
              "Invalid version in object '{}', only 1 is supported", oname);
    PR_RET_IF(hdr->type != OBJ_SUPERBLOCK, std::nullopt,
              "Obj '{}' not a superblock", oname);

    parsed_superblock ret;
    super_hdr *superblk = (super_hdr *)(hdr + 1);

    decode_offset_len<u32>((char *)hdr, superblk->ckpts_offset,
                           superblk->ckpts_len, ret.ckpts);
    decode_offset_len_ptr<clone_info>((char *)hdr, superblk->clones_offset,
                                      superblk->clones_len, ret.clones);
    decode_offset_len_ptr<snap_info>((char *)hdr, superblk->snaps_offset,
                                     superblk->snaps_len, ret.snaps);

    ret.superblock_buf = *buf;
    uuid_copy(ret.uuid, hdr->vol_uuid);
    ret.vol_size = superblk->vol_size * 512;

    return ret;
}

opt<parsed_data_hdr> object_reader::read_data_hdr(std::string oname)
{
    auto hdr = fetch_object_header(oname);
    PASSTHRU_NULLOPT(hdr);

    parsed_data_hdr h;
    h.hdr = (common_obj_hdr *)h.buf.data();
    PR_RET_IF(h.hdr->type != OBJ_LOGDATA, std::nullopt,
              "Invalid object type in '{}'", oname);
    h.data_hdr = (obj_data_hdr *)(hdr->data() + sizeof(common_obj_hdr));

    auto buf = (char *)hdr->data();
    decode_offset_len_ptr<obj_cleaned>(buf, h.data_hdr->objs_cleaned_offset,
                                       h.data_hdr->objs_cleaned_len, h.cleaned);
    decode_offset_len_ptr<data_map>(buf, h.data_hdr->data_map_offset,
                                    h.data_hdr->data_map_len, h.data_map);

    h.buf = std::move(*hdr);
    return h;
}

opt<parsed_checkpoint> object_reader::read_checkpoint(std::string oname)
{
    auto hdr = fetch_object_header(oname);
    PASSTHRU_NULLOPT(hdr);

    parsed_checkpoint ret;
    ret.hdr = (common_obj_hdr *)hdr->data();
    PR_RET_IF(ret.hdr->type != OBJ_CHECKPOINT, std::nullopt,
              "Invalid object type it '{}'", oname);
    ret.ckpt_hdr = (obj_ckpt_hdr *)(&hdr->at(sizeof(common_obj_hdr)));

    auto buf = (char *)hdr->data();
    decode_offset_len<u32>(buf, ret.ckpt_hdr->ckpts_offset,
                           ret.ckpt_hdr->ckpts_len, ret.ckpts);

    decode_offset_len_ptr<ckpt_obj>(buf, ret.ckpt_hdr->objs_offset,
                                    ret.ckpt_hdr->objs_len, ret.objects);
    decode_offset_len_ptr<deferred_delete>(buf, ret.ckpt_hdr->deletes_offset,
                                           ret.ckpt_hdr->deletes_len,
                                           ret.deletes);
    decode_offset_len_ptr<ckpt_mapentry>(buf, ret.ckpt_hdr->map_offset,
                                         ret.ckpt_hdr->map_len, ret.dmap);
    ret.buf = std::move(*hdr);
    return ret;
}