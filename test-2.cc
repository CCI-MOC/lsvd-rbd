/* translated from test1.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>		// mkdir

#include <algorithm>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;


#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fake_rbd.h"
#include "lsvd_debug.h"
#include "objects.h"
#include "lsvd_types.h"

std::string img = "/tmp/bkt/obj";
std::string dir = "/tmp/bkt";

void cleanup(void) {
    if (access(dir.c_str(), R_OK|W_OK|X_OK) < 0)
	mkdir(dir.c_str(), 0777);
    for (const auto & entry : fs::directory_iterator(dir))
	unlink(entry.path().c_str());
}

uuid_t uuid;

void write_super(std::string &name, int ckpt, int next_obj) {
    obj_hdr h = {LSVD_MAGIC, 1, {0}, LSVD_SUPER, 0, 8, 0};
    int size = 10*1024*1024 / 512; // volume size in sectors
    size_t offset = sizeof(obj_hdr) + sizeof(super_hdr);
    int ckpt_len = (ckpt == 0) ? 0 : 4;
    super_hdr sh = {(uint64_t) size,
		    0,		// total sectors in numbered objects
		    0,		// live sectors in them
		    (uint32_t)next_obj,
		    (uint32_t)offset, // of ckpts
		    (uint32_t)ckpt_len,
		    0, 0,	// clone bases: offset, len
		    0, 0};	// snapshots: offset, len
    char buf[4096], *p = buf;
    memset(buf, 0, sizeof(buf));
    memcpy(p, (char*)&h, sizeof(h));
    p += sizeof(h);
    memcpy(p, (char*)&sh, sizeof(sh));
    p += sizeof(sh);
    memcpy(p, (char*)&ckpt, ckpt_len);
    p += ckpt_len;

    FILE *fp = fopen(name.c_str(), "wb");
    fwrite(buf, 1, 4096, fp);
    fclose(fp);
}

char pad[512];

void write_data_1(std::string &name, int ckpt, int seq) {
    std::string pattern = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char *data = (char*)malloc(26*79*512), *ptr = data;
    for (int i = 0; i < 79; i++) {
	for (char & c : pattern) {
	    memset(ptr, c, 512);
	    ptr += 512;
	}
    }
    int data_len = 1024*1024;
    int data_sectors = data_len / 512;

    std::vector<data_map> entries;
    for (int i = 0; i < 4096; i += 2)
	entries.push_back((data_map){(uint64_t)i, 1});
    size_t map_len = entries.size() * sizeof(data_map);
    char map_buf[map_len], *p = map_buf;
    for (auto e : entries) {
	*(data_map*)p = e;
	p += sizeof(data_map);
    }
    size_t hdr_bytes = sizeof(obj_hdr) + sizeof(obj_data_hdr) + map_len;
    if (ckpt)
	hdr_bytes += 4;
    int hdr_sectors = (hdr_bytes + 511) / 512;

    obj_hdr h = {LSVD_MAGIC, 1, {0}, LSVD_DATA, (uint32_t)seq,
		 (uint32_t)hdr_sectors, (uint32_t)data_sectors};
    size_t offset = sizeof(obj_hdr) + sizeof(obj_data_hdr);
    size_t ckpt_len = ckpt ? 4 : 0;
    obj_data_hdr dh = {0,	// last data object
		       (uint32_t)offset,	// ckpts offset
		       (uint32_t)ckpt_len,
		       0, 0,	// objects cleaned
		       (uint32_t)(offset + ckpt_len),
		       (uint32_t)map_len};

    size_t pad_len = hdr_sectors * 512 - hdr_bytes;
    FILE *fp = fopen(name.c_str(), "wb");
    fwrite((char*)&h, sizeof(h), 1, fp);
    fwrite((char*)&dh, sizeof(dh), 1, fp);
    if (ckpt)
	fwrite((char*)&ckpt, 4, 1, fp);
    fwrite(map_buf, map_len, 1, fp);
    fwrite(pad, pad_len, 1, fp);
    fwrite(data, data_len, 1, fp);
    fclose(fp);
    free(data);
}

bool verify_bytes(const char *ptr, off_t start, off_t len, int val) {
    for (int i = 0; i < len; i++)
	if (ptr[start+i] != val)
	    return false;
    return true;
}

TEST_CASE("Volume size") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    int sz = xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    CHECK(sz == 10*1024*1024);
    xlate_close(xlate);
}

TEST_CASE("Recover") {
    cleanup();
    write_super(img, 0, 1);
    std::string oname = img + ".00000001";
    write_data_1(oname, 0, 1);

    _dbg *xlate;
    int sz = xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    CHECK(sz == 10*1024*1024);
    CHECK(xlate_seq(xlate) == 2);

    char buf[512];
    xlate_read(xlate, buf, 0, 512);
    CHECK(verify_bytes(buf, 0, 512, 'A'));
    
    xlate_close(xlate);
}

TEST_CASE("Persist") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    std::vector<char> d(4096, 'X');
    xlate_write(xlate, d.data(), 0, d.size());
    xlate_write(xlate, d.data(), 8192, d.size());
    xlate_flush(xlate);
    usleep(100000);

    auto obj1 = img + ".00000001";
    CHECK(access(obj1.c_str(), R_OK) == 0);
    xlate_close(xlate);

    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    char buf[4096];
    xlate_read(xlate, buf, 0, 4096);
    CHECK(verify_bytes(buf, 0, 4096, 'X'));

    xlate_close(xlate);
}

struct x_mapentry {
    int64_t lba : 36;
    int64_t len : 28;
    int32_t obj;
    int32_t offset;
    bool operator==(x_mapentry const& rhs) const {
	return !memcmp((void*)this, (void*)&rhs, sizeof(rhs));
    }
};

void read_ckpt(const char *img,
	       obj_hdr *hdr,
	       obj_ckpt_hdr *ckpt_hdr,
	       std::vector<int> &ckpts,
	       std::vector<x_mapentry> &entries,
	       std::vector<ckpt_obj> &objs) {
    FILE *fp = fopen(img, "rb");
    fread(hdr, sizeof(*hdr), 1, fp);
    fread(ckpt_hdr, sizeof(*ckpt_hdr), 1, fp);
    CHECK(hdr->magic == LSVD_MAGIC);
    CHECK(hdr->type == LSVD_CKPT);

    int n_ckpts = ckpt_hdr->ckpts_len / 4;
    int _ckpts[n_ckpts];
    fread((void*)_ckpts, ckpt_hdr->ckpts_len, 1, fp);
    for (int i = 0; i < n_ckpts; i++)
	ckpts.push_back(_ckpts[i]);

    CHECK(ftell(fp) == ckpt_hdr->objs_offset);
    int n_objs = ckpt_hdr->objs_len / sizeof(ckpt_obj);
    ckpt_obj _objs[n_objs];
    fread((void*)_objs, ckpt_hdr->objs_len, 1, fp);
    for (int i = 0; i < n_objs; i++)
	objs.push_back(_objs[i]);

    CHECK(ftell(fp) == ckpt_hdr->map_offset);
    int n_exts = ckpt_hdr->map_len / sizeof(ckpt_mapentry);
    x_mapentry _map_entries[n_exts];
    fread((void*)_map_entries, ckpt_hdr->map_len, 1, fp);
    for (int i = 0; i < n_exts; i++)
	entries.push_back(_map_entries[i]);

}

extern std::string hex(uint32_t n);

TEST_CASE("Checkpoint") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);

    std::vector<char> d(4096, 'X');
    xlate_write(xlate, d.data(), 0, d.size());
    xlate_write(xlate, d.data(), 8192, d.size());
    xlate_write(xlate, d.data(), 4096, d.size());

    int n = xlate_checkpoint(xlate);
    obj_hdr          hdr;
    obj_ckpt_hdr     ckpt_hdr;
    std::vector<int> ckpts;
    std::vector<x_mapentry> entries;
    std::vector<ckpt_obj> objs;

    std::string ckpt_name = img + "." + hex(n);
    
    read_ckpt(ckpt_name.c_str(), &hdr, &ckpt_hdr, ckpts, entries, objs);
    CHECK(hdr.magic == LSVD_MAGIC);
    CHECK(hdr.type == LSVD_CKPT);

    CHECK(ckpts.size() == 1);
    CHECK(ckpts[0] == 2);
    
    std::vector<x_mapentry> test_entries {{0,8,1,1}, {8,8,1,17}, {16,8,1,9}};
    CHECK(std::equal(entries.begin(), entries.end(), test_entries.begin()));
    
    xlate_close(xlate);
}

TEST_CASE("Flush thread") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    std::string obj1 = img + ".00000001";
    CHECK(access(obj1.c_str(), R_OK) < 0);

    std::vector<char> d(4096,'Y');
    xlate_write(xlate, d.data(), 0, d.size());
    xlate_write(xlate, d.data(), 8192, d.size());
    CHECK(access(obj1.c_str(), R_OK) < 0);

    sleep(3);
    CHECK(access(obj1.c_str(), R_OK) == 0);
    xlate_close(xlate);
    CHECK(access(obj1.c_str(), R_OK) == 0);

    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);
    char buf[4096];
    memset(buf, 0xff, sizeof(buf));
    xlate_read(xlate, buf, 0, 4096);
    CHECK(verify_bytes(buf, 0, 4096, 'Y'));

    memset(buf, 0xff, sizeof(buf));
    xlate_read(xlate, buf, 4096, 4096);
    CHECK(verify_bytes(buf, 0, 4096, 0));

    memset(buf, 0xff, sizeof(buf));
    xlate_read(xlate, buf, 8192, 4096);
    CHECK(verify_bytes(buf, 0, 4096, 'Y'));
    
    xlate_close(xlate);
}

/* object list persisted correctly in checkpoint */
TEST_CASE("Object list persistence") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);

    std::vector<char> d(4096,'Z');
    xlate_write(xlate, d.data(), 4096, d.size());
    std::string obj1 = img + ".00000001";    
    CHECK(access(obj1.c_str(), R_OK) < 0);
    
    xlate_flush(xlate);

    CHECK(access(obj1.c_str(), R_OK) == 0);
    std::string obj2 = img + ".00000002";    
    CHECK(access(obj2.c_str(), R_OK) < 0);

    int n = xlate_checkpoint(xlate);
    CHECK(n == 2);
    CHECK(access(obj2.c_str(), R_OK) == 0);
    
    obj_hdr          hdr;
    obj_ckpt_hdr     ckpt_hdr;
    std::vector<int> ckpts;
    std::vector<x_mapentry> entries;
    std::vector<ckpt_obj> objs;
    read_ckpt(obj2.c_str(), &hdr, &ckpt_hdr, ckpts, entries, objs);
    CHECK(hdr.magic == LSVD_MAGIC);
    CHECK(hdr.type == LSVD_CKPT);

    CHECK(objs.size() == 1);
    auto o = objs[0];
    CHECK(o.seq == 1);
    CHECK(o.data_sectors == 8);
    CHECK(o.live_sectors == 8);

    xlate_close(xlate);
}

TEST_CASE("More object/checkpoint tests") {
    cleanup();
    write_super(img, 0, 1);
    _dbg *xlate;
    xlate_open((char*)img.c_str(), 1, true, (void**)&xlate);

    std::vector<char> d(4096,'Z');
    xlate_write(xlate, d.data(), 4096, d.size());
    xlate_flush(xlate);

    int n = xlate_checkpoint(xlate);
    CHECK(n == 2);
    CHECK(access((img + ".00000002").c_str(), R_OK) == 0);

    std::vector<char> d2(4096,'Q');
    xlate_write(xlate, d2.data(), 4096, d2.size());
    xlate_flush(xlate);

    CHECK(access((img + ".00000003").c_str(), R_OK) == 0);
    CHECK(access((img + ".00000004").c_str(), R_OK) < 0);
    n = xlate_checkpoint(xlate);
    CHECK(access((img + ".00000004").c_str(), R_OK) == 0);
    
    obj_hdr          hdr;
    obj_ckpt_hdr     ckpt_hdr;
    std::vector<int> ckpts;
    std::vector<x_mapentry> entries;
    std::vector<ckpt_obj> objs;
    read_ckpt((img + "." + hex(n)).c_str(), &hdr, &ckpt_hdr,
	      ckpts, entries, objs);
    CHECK(objs.size() == 2);
    CHECK(objs[0].seq == 1);
    CHECK(objs[1].seq == 3);
    CHECK(objs[0].data_sectors == 8);
    CHECK(objs[0].live_sectors == 0);
    CHECK(objs[1].data_sectors == 8);
    CHECK(objs[1].live_sectors == 8);

    std::vector<char> buf(4096,0xFF);
    xlate_read(xlate, buf.data(), 4096, 4096);
    CHECK(verify_bytes(buf.data(), 0, 4096, 'Q'));

    xlate_close(xlate);
}
