#ifndef REQUEST_H
#define REQUEST_H

class request {
public:
    virtual bool is_done(void) = 0;
    virtual void run(void *parent) = 0;
    virtual void notify(void) = 0;
    virtual ~request(){}
};

#endif
