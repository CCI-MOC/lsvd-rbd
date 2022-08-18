// file:	request.h
// description:	This file contains the request interface used by all other request classes in the LSVD system
// 

#ifndef REQUEST_H
#define REQUEST_H

// The request class interface is used in order to perform writes and reads at different levels.
// When a write or a read is needed to be completed, a request is created for each level necessary of that
// level in an appropriate heirarchy. Otherwise, the request interface is simple:
//	is_done: this function simply returns the status of the current request, returning true if request has completed
//	run:	 this function runs the request, potentially creating lower level requests if necessary
//		 a pointer to the parent request is input in order to be able to notify the higher level requests
//		 when the lower level request completes
//	notify:	 This function is used to notify the parent request that the current request has completed
class request {
public:
    virtual bool is_done(void) = 0;
    virtual void run(void *parent) = 0;
    virtual void notify(void) = 0;
    virtual ~request(){}
};

#endif
