#include <cassert>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <queue>
#include <rados/librados.h>
#include <stdio.h>

#include "lsvd_types.h"

#include "backend.h"
#include "extent.h"
#include "objects.h"
#include "objname.h"
#include "request.h"
#include "smartiov.h"

struct fetcher : public trivial_request {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

  public:
    int objnum;
    int sectors;
    char objname[128];
    char filename[128];
    char *buf = NULL;
    request *req = NULL;

    void notify(request *child)
    {
        if (child != NULL)
            child->release();
        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        assert(fd >= 0);
        write(fd, buf, sectors * 512L);
        close(fd);
        free(buf);
        std::unique_lock lk(m);
        done = true;
        cv.notify_all();
    }

    void wait_for(void)
    {
        std::unique_lock lk(m);
        while (!done)
            cv.wait(lk);
    }
};

void write_obj(char *dir, char *xprefix, int seq, char *buf, size_t bytes)
{
    char filename[128];
    sprintf(filename, "%s/%s.%08x", dir, xprefix, seq);
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    assert(fd >= 0);
    write(fd, buf, bytes);
    close(fd);
}

int main(int argc, char **argv)
{
    char *prefix = argv[1];
    char *xprefix = strchr(prefix, '/') + 1;
    char *dir = argv[2];

    auto be = make_rados_backend(NULL);
    auto buf = (char *)malloc(4096);

    be->read_object(prefix, buf, 4096, 0);

    auto h = (obj_hdr *)buf;
    auto sb = (super_hdr *)(h + 1);
    assert(h->magic == LSVD_MAGIC);

    printf("size: %d (%.2f GB %.2f GiB)\n", sb->vol_size,
           sb->vol_size * 512.0 / 1e9,
           sb->vol_size * 512.0 / 1024 / 1024 / 1024);
    int *ck = (int *)(buf + sb->ckpts_offset);
    int nck = sb->ckpts_len / 4;
    int last_ck = 0;
    for (int i = 0; i < nck; i++)
        last_ck = ck[i];

    auto buf2 = (char *)malloc(4096);
    char name[128];
    sprintf(name, "%s.%08x", prefix, last_ck);
    be->read_object(name, buf2, 4096, 0);
    h = (obj_hdr *)buf2;
    printf("ckpt %08x sectors %d %d\n", last_ck, h->hdr_sectors,
           h->data_sectors);

    size_t bytes = 512 * h->hdr_sectors;
    auto ckpt_buf = (char *)malloc(bytes);
    be->read_object(name, ckpt_buf, bytes, 0);

    write_obj(dir, xprefix, last_ck, ckpt_buf, bytes);

    h = (obj_hdr *)ckpt_buf;
    auto cp = (obj_ckpt_hdr *)(h + 1);

    auto objs = (ckpt_obj *)(ckpt_buf + cp->objs_offset);
    int n_objs = cp->objs_len / sizeof(*objs);

    std::queue<fetcher *> q;

    for (int i = 0; i < n_objs; i++) {
        auto f = new fetcher;
        int seq = objs[i].seq;
        f->objnum = seq;
        f->sectors = objs[i].hdr_sectors + objs[i].data_sectors;
        sprintf(f->objname, "%s.%08x", prefix, seq);
        sprintf(f->filename, "%s/%s.%08x", dir, xprefix, seq);
        size_t bytes = f->sectors * 512L;
        f->buf = (char *)malloc(bytes);
        f->req = be->make_read_req(f->objname, 0, f->buf, bytes);
        f->req->run(f);
        q.push(f);

        while (q.size() > 8) {
            auto _f = q.front();
            q.pop();
            _f->wait_for();
            printf("%s\n", _f->objname);
            delete _f;
        }
    }

    while (q.size() > 0) {
        auto _f = q.front();
        q.pop();
        _f->wait_for();
        delete _f;
    }
}
