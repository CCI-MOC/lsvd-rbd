/*
 * file:        request.h
 * description: generic inter-layer request mechanism
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */
#pragma once

#include "utils.h"
#include <atomic>

/* generic interface for requests.
 *  - run(parent): begin execution
 *  - notify(rv): notification of completion
 *  - TODO: wait(): wait for completion
 */
class request
{
  public:
    virtual void wait() = 0;
    virtual void run(request *parent) = 0;
    virtual void notify(request *child) = 0;
    virtual void release() = 0;
    virtual ~request() {}
    request() {}
};

/* for callback-only request classes
 */
class trivial_request : public request
{
  public:
    trivial_request() {}
    ~trivial_request() {}
    virtual void notify(request *child) = 0;
    void wait() {}
    void run(request *parent) {}
    void release() {}
};

/**
 * This is a workaround for the existing codebase doing ad-hoc lifetime
 * management for requests.
 *
 * From what I understand, the original codebase has requests that delete
 * themselves on `release()`, which the parent is reponsible for calling on
 * the child.
 *
 * But sometimes you have `release()` called before you can actually free
 * everything, so you need to do a self-reference count to prevent UAFs. The
 * self refcount is done in an ad-hoc manner with all sorts of state transitions
 * and it's a mess.
 *
 * The idea with this class is that each request really starts with two
 * referees: the completion event and the parent request. So what we do is we
 * start with refcount 2, and decrement on both release() and on notify().
 *
 * This is backwards compatible with the existing request structure; in an
 * ideal world, we would pass shared_from_this shared_ptrs to both the parent
 * and the child and they would decrement the refcount when they're done,
 * but that would require rewriting the lifetime management of all requests
 * in the entire codebase. This way is a drop-in replacement that has minimal
 * impact on everything else.
 *
 * TODO implement wait() if neccessary with atomic_flags
 */
class self_refcount_request : public request
{
  protected:
    std::atomic_int refcount = 2;

    inline void dec_and_free()
    {
        auto old = refcount.fetch_sub(1, std::memory_order_seq_cst);
        if (old == 1)
            delete this;
    }

    /**
     * Might move this into the destructor and lift subrequests into this class
     */
    inline void free_child(request *child)
    {
        if (child)
            child->release();
    }

  public:
    inline virtual void wait() override { UNIMPLEMENTED(); }
    inline virtual void release() override { dec_and_free(); }
};
