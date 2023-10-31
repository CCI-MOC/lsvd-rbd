CXX = g++-12
CC = gcc-12
BUILD_DIR = build

.DEFAULT_GOAL := debug

# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
# -fno-omit-frame-pointer is so perf gets good stack traces
CFLAGS = -ggdb3 -Wall $(OPT)
CXXFLAGS = -std=c++17 -ggdb3 -Wall $(OPT) -fno-omit-frame-pointer -fPIC
LDFLAGS = -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados -lfmt -l:liburing.a
LDFLAGS += -fuse-ld=mold -Wl,-rpath=/usr/lib/liburing.so.2.5
SOFLAGS = -shared -fPIC

debug: CXXFLAGS += -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
debug: CXXFLAGS += -fsanitize=address -static-libasan
# debug: CXXFLAGS += -fsanitize=thread
debug: CXXFLAGS += -Wall -Wextra -Wdouble-promotion -Wno-sign-conversion -Wno-conversion -Wno-unused-parameter
debug: CXXFLAGS += -O0 -fno-omit-frame-pointer -fno-inline
nosan: CXXFLAGS += -O0 -fno-omit-frame-pointer -fno-inline
release: CXXFLAGS += -O3

debug: liblsvd.so imgtool lsvd_rnd_test
nosan: liblsvd.so imgtool lsvd_rnd_test
release: liblsvd.so

CPP = $(wildcard *.cc)
OBJS = $(CPP:%.cc=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

$(BUILD_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP -o $@ -c $< $(CXXFLAGS)

LSVD_DEPS = objects.o translate.o io.o read_cache.o config.o mkcache.o \
	nvme.o write_cache.o file_backend.o \
	rados_backend.o lsvd_debug.o liblsvd.o
LSVD_OBJS = $(LSVD_DEPS:%.o=$(BUILD_DIR)/%.o)

liblsvd.so: $(LSVD_OBJS)
	$(CXX) $(SOFLAGS) -o $@ $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

imgtool: imgtool.o $(LSVD_OBJS) liblsvd.so
	$(CXX) -o $@ imgtool.o $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

thick-image: thick-image.o $(LSVD_OBJS) liblsvd.so
	$(CXX) -o $@ thick-image.o $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

lsvd_rnd_test: lsvd_rnd_test.o $(LSVD_OBJS) liblsvd.so
	$(CXX) -o $@ lsvd_rnd_test.o $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

lsvd_crash_test: lsvd_crash_test.o $(LSVD_OBJS) liblsvd.so
	$(CXX) -o $@ lsvd_crash_test.o $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

test-1: test-1.o $(OBJS)
	$(CXX) -o $@ test-1.o $(OBJS) $(CXXFLAGS)

test-2: test-2.o $(OBJS)
	$(CXX) -o $@ test-2.o $(OBJS) $(CXXFLAGS)

test7: test7.o $(OBJS)
	$(CXX) -o $@ test7.o $(OBJS) $(CRC_OBJS) $(CXXFLAGS)

test8: test8.o $(OBJS)
	$(CXX) -o $@ test8.o $(OBJS) $(CRC_OBJS) $(CXXFLAGS)

test9: test9.o $(OBJS)
	$(CXX) -o $@ test9.o $(OBJS) $(CRC_OBJS) $(CXXFLAGS)

test10: test10.o $(OBJS)
	$(CXX) -o $@ test10.o $(OBJS) $(CRC_OBJS) $(CXXFLAGS)

bdus: bdus.o $(OBJS)
	$(CXX) $(OBJS) bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados -laio -luuid

unit-test: unit-test.cc extent.h
	$(CXX) $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	$(CC) $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk $(OBJS) $(DEPS) *.o *.d test7 test8 imgtool

install-deps:
	sudo apt install libfmt-dev libaio-dev librados-dev mold
	echo "You'll have to compile the latest version of liburing yourself"

