// file:	request.h
// description:	This file contains the request interface used by all other request classes in the LSVD system
// 

#ifndef REQUEST_H
#define REQUEST_H

/* generic interface for requests.
 *  - run(parent): begin execution
 *  - notify(rv): notification of completion
 *  - TODO: wait(): wait for completion
 */
class request {
public:
    virtual sector_t lba() = 0;
    virtual smartiov *iovs() = 0;
    
    virtual bool is_done(void) = 0;
    virtual void run(request *parent) = 0;
    virtual void notify(void) = 0;
    virtual ~request(){}
};

#endif
