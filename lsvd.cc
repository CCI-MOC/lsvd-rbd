#include "extent.cc"

extmap::objmap   obj_map;
extmap::cachemap cache_map;
extmap::bufmap   buf_map;

#include <stack>
std::stack<char*> free_bufs;
char *current_buf, *current_ptr;
size_t buf_size;

void write(int64_t lba, char *buf, int len)
{
    if (len + (current_ptr - current_buf) > buf_size) {
	char *buf = current_buf;
	size_t len = current_ptr - current_buf;
	if (free_bufs.size() == 0)
	    current_buf = malloc(buf_size);
	else {
	    current_buf = free_bufs.top();
	    free_bufs.pop();	// why can't C++ stacks work right?
	}
	current_ptr = current_buf;
    }
}


int main(int argc, char **argv)
{
}
