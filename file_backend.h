// file:	file_backend.h
// description: File_backend is a class which hosts functions for specifically writing objects to files and managing
//		data within files instead of the disk.
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef FILE_BACKEND_H
#define FILE_BACKEND_H

class file_backend : public backend {
    char *prefix;
    std::mutex m;
    std::map<int,int> cached_fds;
    std::queue<int>   cached_nums;
    static const int  fd_cache_size = 500;

// get_cached_fd :	returns the file descriptor of the file with the defined inputted seq. Checks
//			cached fd's first and returns the fd for the cachedm and otherwise creates
//			a file with the specified seq and returns the file descriptor for that file
    int get_cached_fd(int seq);

    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;

public:
// file_backend :	Constructor for the file_backend
    file_backend(const char *_prefix) {
	prefix = strdup(_prefix);
	e_io_running = true;
	io_queue_init(64, &ioctx);
	const char *name = "file_backend_cb";
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
    }

// write_object :	Opens a file with the given name, and writes the iov to the file, closes file, returns
//			the output of the writev operation
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);

// write_numbered_object : 	calls write_object using a name based on the seq number inputted
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);

// delete_numbered_object :	unlinks the object with the inputted seq number
    void delete_numbered_object(int seq);

// read_object :	Opens file determined by name, preads it based on buf, len, and offset, and returns
//			value of pread on success.
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);

// read_numbered_object :	calls read_numbered_objectv with buf and len contained in an iov
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);

// read_numbered_objectv :	calls get_cached_fd to get the file descriptor with seq, then calls
//				preadv and returns the value of preadv
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);

// aio_read_num_object :	calls get_cached_fd to get the file descriptor with seq, then creates an e_iocb
//                              then calls the io.h functions of async read and submit functions
    int aio_read_num_object(int seq, char *buf, size_t len,
			    size_t offset, void (*cb)(void*), void *ptr);

// aio_write_numbered_objects :	Opens a file based on seq, wraps the callback and ptr inputted, and then calls
//				the async write and submit operation from io.h
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr);
// ~file_backend :	Deconstructor for file_backend
    ~file_backend() {
	free((void*)prefix);
	for (auto it = cached_fds.begin(); it != cached_fds.end(); it++)
	    close(it->second);

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }
// object_name :	returns the name of the file name based on seq whether it exists or not
    std::string object_name(int seq);
};


#endif
