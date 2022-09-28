#ifndef SEND_WRITE_REQUEST_H
#define SEND_WRITE_REQUEST_H

class write_cache;

class send_write_request : public request {
    std::atomic<int> reqs = 0;

    sector_t      plba;
    std::vector<request*> *work = NULL;
    nvme_request *r_data = NULL;
    char         *hdr = NULL;
    smartiov     *data_iovs = NULL;

    nvme_request *r_pad = NULL;
    char         *pad_hdr = NULL;
    smartiov     *pad_iov = NULL;

    write_cache  *wcache = NULL;

public:
    send_write_request(std::vector<request*> *w, page_t n_pages, page_t page,
                       page_t n_pad, page_t pad, write_cache *wcache);
    ~send_write_request();

    sector_t lba();
    smartiov *iovs();
    bool is_done(void);
    void run(request *parent);
    void notify(void);
};

#endif
