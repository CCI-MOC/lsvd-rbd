#pragma once

#include "utils.h"

/* generic interface for requests.
 *  - run(parent): begin execution
 *  - notify(rv): notification of completion
 *  - TODO: wait(): wait for completion
 */
class request
{
  public:
    virtual void wait() = 0;
    virtual void run(sptr<request> parent) = 0;
    virtual void notify() = 0;
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
    virtual void notify() = 0;
    void wait() {}
    void run(sptr<request> parent) {}
    void release() {}
};
