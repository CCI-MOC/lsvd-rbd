#include "extent.cc"

extmap::objmap   obj_map;
extmap::cachemap cache_map;
extmap::bufmap   buf_map;

#include <stack>
std::stack<char*> free_bufs;
char *current_buf, *current_ptr;
size_t buf_size;

#include "thread_pool.h"

int main(int argc, char **argv)
{
}
