#ifndef SEND_WRITE_REQUEST_H
#define SEND_WRITE_REQUEST_H

class write_cache;

class send_write_request : public request {
public:
    std::atomic<int> reqs = 0;

    sector_t      plba;
    std::vector<cache_work*> *work = NULL;
    nvme_request *r_data = NULL;
    char         *hdr = NULL;
    smartiov     *iovs = NULL;

    nvme_request *r_pad = NULL;
    char         *pad_hdr = NULL;
    smartiov     *pad_iov = NULL;

    write_cache  *wcache = NULL;
    
    send_write_request(std::vector<cache_work*> *w, page_t n_pages, page_t page,
                       page_t n_pad, page_t pad, write_cache *wcache);

    bool is_done();
    void run(void* parent);
    void notify();
    ~send_write_request();
};

#endif
