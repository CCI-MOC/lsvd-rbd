/*
 * TODO: straighten out header files some more
 */

#ifndef __IMAGE_H__
#define __IMAGE_H__

struct lsvd_completion;

struct event_socket {
    int socket;
    int type;
public:
    event_socket(): socket(-1), type(0) {}
    bool is_valid() const { return socket != -1; }
    int init(int fd, int t) {
	socket = fd;
	type = t;
	return 0;
    }
    int notify() {
	int rv;
	switch (type) {
	case EVENT_TYPE_PIPE:
	{
	    char buf[1] = {'i'}; // why 'i'???
	    rv = write(socket, buf, 1);
	    rv = (rv < 0) ? -errno : 0;
	    break;
	}
	case EVENT_TYPE_EVENTFD:
	{
	    uint64_t value = 1;
	    rv = write(socket, &value, sizeof (value));
	    rv = (rv < 0) ? -errno : 0;
	    break;
	}
	default:
	    rv = -1;
	}
	return rv;
    }
};

class backend;
class objmap;
class translate;
class write_cache;
class read_cache;
struct j_super;

struct fake_rbd_image {
    std::mutex   m;
    backend     *io;
    objmap      *omap;
    translate   *lsvd;
    write_cache *wcache;
    read_cache  *rcache;
    ssize_t      size;          // bytes
    int          fd;            // cache device
    j_super     *js;            // cache page 0

    event_socket ev;
    std::queue<rbd_completion_t> completions;
};

#endif
