#include <cerrno>
#include <liburing.h>
#include <mutex>
#include <stop_token>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <uuid/uuid.h>

#include "folly/experimental/coro/Mutex.h"
#include "lsvd_types.h"
#include "nvme.h"
#include "request.h"
#include "smartiov.h"
#include "utils.h"

void do_log(const char *, ...);

class nvme_uring : public nvme
{
  private:
    io_uring ring;
    std::mutex uring_mtx;
    std::thread uring_cqe_worker;

    const char *name;
    std::atomic_bool cqe_worker_should_continue = true;

  public:
    int fd;

    nvme_uring(int fd_, const char *name_) : name(name_)
    {
        fd = fd_;
        auto ioret = io_uring_queue_init(256, &ring, 0);
        assert(ioret == 0);
        // debug("io_uring queue initialised");

        uring_cqe_worker =
            std::thread(&nvme_uring::uring_completion_worker, this, name);
    }

    ~nvme_uring()
    {
        cqe_worker_should_continue = false;
        uring_cqe_worker.join();
        io_uring_queue_exit(&ring);
    }

    int read(void *buf, size_t count, off_t offset)
    {
        return pread(fd, buf, count, offset);
    }

    int write(const void *buf, size_t count, off_t offset)
    {
        return pwrite(fd, buf, count, offset);
    }

    int writev(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return pwritev(fd, iov, iovcnt, offset);
    }

    int readv(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return preadv(fd, iov, iovcnt, offset);
    }

    request *make_write_request(smartiov *iov, size_t offset)
    {
        assert(offset != 0);
        return (request *)new nvme_uring_request(iov, offset, OP_WRITE, this);
    }

    request *make_write_request(char *buf, size_t len, size_t offset)
    {
        assert(offset != 0);
        return (request *)new nvme_uring_request(buf, len, offset, OP_WRITE,
                                                 this);
    }

    request *make_read_request(smartiov *iov, size_t offset)
    {
        return (request *)new nvme_uring_request(iov, offset, OP_READ, this);
    }

    request *make_read_request(char *buf, size_t len, size_t offset)
    {
        return (request *)new nvme_uring_request(buf, len, offset, OP_READ,
                                                 this);
    }

    void uring_completion_worker(const char *name)
    {
        pthread_setname_np(pthread_self(), name);

        __kernel_timespec timeout = {0, 1000 * 100}; // 100 microseconds
        while (cqe_worker_should_continue.load(std::memory_order_seq_cst)) {
            io_uring_cqe *cqe;
            auto res = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);
            // auto res = io_uring_wait_cqe(&ring, &cqe);

            // TODO handle error
            if (res != 0)
                continue;

            nvme_uring_request *req =
                static_cast<nvme_uring_request *>(io_uring_cqe_get_data(cqe));

            if (cqe->res < 0)
                req->on_fail(-1 * cqe->res);
            else
                req->on_complete(cqe->res);

            io_uring_cqe_seen(&ring, cqe);
        }
    }

  private:
    class nvme_uring_request : public self_refcount_request
    {
      private:
        nvme_uring *nvmu_;
        smartiov iovs_;
        lsvd_op op_;
        size_t offset_;

        // other bookkeeping, copied from the aio version
        request *parent_;

      public:
        nvme_uring_request(smartiov *iovs, size_t offset, lsvd_op op,
                           nvme_uring *nvmu)
            : nvmu_(nvmu), iovs_(iovs->data(), iovs->size()), op_(op),
              offset_(offset)
        {
        }

        nvme_uring_request(char *buf, size_t len, size_t offset, lsvd_op op,
                           nvme_uring *nvmu)
            : nvmu_(nvmu), iovs_(buf, len), op_(op), offset_(offset)
        {
        }

        ~nvme_uring_request() {}

        void run(request *parent)
        {
            assert(parent != NULL);
            parent_ = parent;

            {
                std::unique_lock lk(nvmu_->uring_mtx);
                io_uring_sqe *sqe = io_uring_get_sqe(&nvmu_->ring);
                assert(sqe != NULL);

                // TODO prep_write instead of writev if iov has only 1 element
                if (op_ == OP_WRITE)
                    io_uring_prep_writev(sqe, nvmu_->fd, iovs_.data(),
                                         iovs_.size(), offset_);
                else if (op_ == OP_READ)
                    io_uring_prep_readv(sqe, nvmu_->fd, iovs_.data(),
                                        iovs_.size(), offset_);
                else
                    assert(false);

                io_uring_sqe_set_data(sqe, this);
                io_uring_submit(&nvmu_->ring);
            }
        }

        void on_complete(int result)
        {
            // TODO figure out error handling
            if (std::cmp_not_equal(result, iovs_.bytes()))
                log_error("nvme uring request completed with partial result");

            parent_->notify(this);
            dec_and_free();
        }

        void on_fail(int errnum)
        {
            // TODO there's no failure path yet so just no-op
            log_error("nvme uring request failed");
            parent_->notify(this);
            dec_and_free();
        }

        void notify(request *child) { UNIMPLEMENTED(); }
    };
};

class file_uring_coro : public fileio
{
  private:
    int fd;
    // eventually we want more than one ring (they're lightweight enough for one
    // per thread), but for now just have one and guard access to it with locks
    io_uring ring;
    folly::coro::Mutex ring_mtx;
    std::jthread cqe_worker_thread;

    void cqe_worker(std::stop_token st)
    {
        __kernel_timespec timeout = {0, 1000 * 1000 * 10}; // 10msec
        while (!st.stop_requested()) {
            io_uring_cqe *cqe;
            auto res = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);

            if (res != 0) // TODO check for timeout and error if not
                continue;

            auto b = static_cast<folly::coro::Promise<s32> *>(
                io_uring_cqe_get_data(cqe));
            b->setValue(cqe->res);
            io_uring_cqe_seen(&ring, cqe);
        }
    }

    auto convert_result(s32 res) -> Result<s32>
    {
        FAILURE_IF_NEGATIVE(res);
        return res;
    }

  public:
    file_uring_coro(int fd, str name) : fd(fd)
    {
        auto ioret = io_uring_queue_init(256, &ring, 0);
        assert(ioret == 0);

        std::jthread(&file_uring_coro::cqe_worker, this);
    }

    ~file_uring_coro()
    {
        cqe_worker_thread.request_stop();
        cqe_worker_thread.join();
        close(fd);
    }

    ResTask<usize> read(usize offset, smartiov &iov) override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<s32>();
        {
            auto l = co_await ring_mtx.co_scoped_lock();
            auto sqe = io_uring_get_sqe(&ring);
            assert(sqe != NULL);

            io_uring_prep_readv(sqe, fd, iov.data(), iov.size(), offset);
            io_uring_sqe_set_data(sqe, &p);
            io_uring_submit(&ring);
        }

        co_return convert_result(co_await std::move(f));
    }

    ResTask<usize> read(usize offset, iovec iov) override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<s32>();
        {
            auto l = co_await ring_mtx.co_scoped_lock();
            auto sqe = io_uring_get_sqe(&ring);
            assert(sqe != NULL);

            io_uring_prep_read(sqe, fd, iov.iov_base, iov.iov_len, offset);
            io_uring_sqe_set_data(sqe, &p);
            io_uring_submit(&ring);
        }

        co_return convert_result(co_await std::move(f));
    }

    ResTask<usize> write(usize offset, smartiov &iov) override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<s32>();
        {
            auto l = co_await ring_mtx.co_scoped_lock();
            auto sqe = io_uring_get_sqe(&ring);
            assert(sqe != NULL);

            io_uring_prep_writev(sqe, fd, iov.data(), iov.size(), offset);
            io_uring_sqe_set_data(sqe, &p);
            io_uring_submit(&ring);
        }

        co_return convert_result(co_await std::move(f));
    }

    ResTask<usize> write(usize offset, iovec iov) override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<s32>();
        {
            auto l = co_await ring_mtx.co_scoped_lock();
            auto sqe = io_uring_get_sqe(&ring);
            assert(sqe != NULL);

            io_uring_prep_write(sqe, fd, iov.iov_base, iov.iov_len, offset);
            io_uring_sqe_set_data(sqe, &p);
            io_uring_submit(&ring);
        }

        co_return convert_result(co_await std::move(f));
    }
};

uptr<fileio> make_fileio(int fd)
{
    return std::make_unique<file_uring_coro>(fd, "file_uring_coro");
}

nvme *make_nvme_uring(int fd, const char *name)
{
    return (nvme *)new nvme_uring(fd, name);
}
