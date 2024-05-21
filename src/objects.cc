#include <algorithm>
#include <cstring>
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
    // Reserve required space ahead of time
    usize req_size = sizeof(common_obj_hdr) + sizeof(super_hdr);
    req_size += checkpoints.size() * sizeof(seqnum_t);
    for (auto &c : clones)
        req_size += sizeof(clone_info) + round_up(c.name.size(), 8);
    // TODO snapshots
    req_size = std::max(req_size, 8ul); // minimum of 4096 bytes
    req_size = round_up(req_size, 512); // round to sector boundary (why??)

    if (buf.size() < req_size)
        buf.resize(req_size);

    auto bufp = buf.data(); // start of buffer
    auto hdrp = (super_hdr *)(bufp + sizeof(common_obj_hdr));

    serialise_common_hdr(buf, OBJ_SUPERBLOCK, 0, req_size / 512, 0, uuid);

    // There are three variable-length arrays in the superblock: checkpoints,
    // snapshots, and clones. The order doesn't matter, but we put clones first
    // since that's effectively immutable. This means that the offset into
    // everything else will not change over the lifetime of an image
    // The checkpoints and snapshots are appended after that

    // Also note that we should make sure that each clone/snapshot is 8-byte
    // aligned in the buffer, as when we read them back to deserialise we end
    // up with a bunch of pointers into the buffer and unaligned pointers will
    // just make all of us sad. Fortunately we have c-style null-terminated
    // strings so we can just pad with more null

    // Part 1: clones. TODO skip on partial serialise
    byte *clonep;
    clonep = bufp + sizeof(common_obj_hdr) + sizeof(super_hdr);
    hdrp->clones_offset = clonep - bufp;
    for (auto &c : clones) {
        auto padded_namelen = round_up(c.name.size(), 8);
        auto record_len = sizeof(clone_info) + padded_namelen;
        auto cip = (clone_info *)clonep;

        cip->last_seq = c.last_seq;
        uuid_copy(cip->vol_uuid, nullptr); // TODO
        cip->name_len = padded_namelen;

        std::memset(cip->name, 0, padded_namelen);
        std::memcpy(cip->name, c.name.c_str(), c.name.size());

        clonep += record_len;
        hdrp->clones_len += record_len;
    }

    // Part 2: checkpoints
    hdrp->ckpts_offset = clonep - bufp;
    hdrp->ckpts_len = checkpoints.size() * sizeof(seqnum_t);
    auto p = (seqnum_t *)(bufp + hdrp->ckpts_offset);
    for (auto &c : checkpoints)
        *p++ = c;

    // Part 3: snapshots
    // TODO implement this when we get around to snapshots
    hdrp->snaps_offset = 0;
    hdrp->snaps_len = 0;
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

/* buf[offset ... offset+len] contains array of type T, with variable
 * length field name_len.
 */
template <class T>
void deserialise_offset_ptr(char *buf, size_t offset, size_t len,
                            std::vector<T *> &vals)
{
    T *p = (T *)(buf + offset), *end = (T *)(buf + offset + len);
    for (; p < end;) {
        vals.push_back(p);
        p = (T *)((char *)p + sizeof(T) + p->name_len);
    }
}

template <typename T> vec<T> deserialise_cpy(byte *buf, usize offset, usize len)
{
    vec<T> ret;
    for (usize i = 0; i < len / sizeof(T); i++) {
        T *p = (T *)(buf + offset + i * sizeof(T));
        ret.push_back(*p);
    }
    return ret;
}

template <typename T>
vec<T *> deserialise_ptrs(byte *buf, usize offset, usize len)
{
    vec<T *> ret;
    for (usize i = 0; i < len / sizeof(T); i++) {
        T *p = (T *)(buf + offset + i * sizeof(T));
        ret.push_back(p);
    }
    return ret;
}

template <typename T>
vec<T *> deserialise_ptrs_with_len(byte *buf, usize offset, usize len)
{
    vec<T *> ret;
    byte *p = buf + offset;
    for (; p < buf + offset + len;) {
        ret.push_back((T *)p);
        p += sizeof(T) + ((T *)p)->name_len;
    }
    return ret;
}

opt<parsed_superblock> object_reader::read_superblock(std::string oname)
{
    auto buf = objstore->read_whole_obj(oname);
    PASSTHRU_NULLOPT(buf);
    auto hdr = (common_obj_hdr *)buf->data();
    auto hbuf = buf->data();

    PR_RET_IF(hdr->magic != LSVD_MAGIC, std::nullopt,
              "Corrupt object; invalid magic at '{}'", oname);
    PR_RET_IF(hdr->version != 1, std::nullopt,
              "Invalid version in object '{}', only 1 is supported", oname);
    PR_RET_IF(hdr->type != OBJ_SUPERBLOCK, std::nullopt,
              "Obj '{}' not a superblock", oname);

    parsed_superblock ret;
    super_hdr *shdr = (super_hdr *)(hdr + 1);

    ret.ckpts = deserialise_cpy<u32>(hbuf, shdr->ckpts_offset, shdr->ckpts_len);
    ret.clones = deserialise_ptrs_with_len<clone_info>(
        hbuf, shdr->clones_offset, shdr->clones_len);
    ret.snaps = deserialise_ptrs_with_len<snap_info>(hbuf, shdr->snaps_offset,
                                                     shdr->snaps_len);

    ret.superblock_buf = *buf;
    uuid_copy(ret.uuid, hdr->vol_uuid);
    ret.vol_size = shdr->vol_size * 512;

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

    auto buf = hdr->data();
    h.cleaned = deserialise_ptrs<obj_cleaned>(
        buf, h.data_hdr->objs_cleaned_offset, h.data_hdr->objs_cleaned_len);
    h.data_map = deserialise_ptrs<data_map>(buf, h.data_hdr->data_map_offset,
                                            h.data_hdr->data_map_len);
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

    auto buf = hdr->data();
    ret.ckpts = deserialise_cpy<u32>(buf, ret.ckpt_hdr->ckpts_offset,
                                     ret.ckpt_hdr->ckpts_len);
    ret.objects = deserialise_ptrs<ckpt_obj>(buf, ret.ckpt_hdr->objs_offset,
                                             ret.ckpt_hdr->objs_len);
    ret.deletes = deserialise_ptrs<deferred_delete>(
        buf, ret.ckpt_hdr->deletes_offset, ret.ckpt_hdr->deletes_len);
    ret.dmap = deserialise_ptrs<ckpt_mapentry>(buf, ret.ckpt_hdr->map_offset,
                                               ret.ckpt_hdr->map_len);
    ret.buf = std::move(*hdr);
    return ret;
}