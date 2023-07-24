CXX = g++
CC = g++

# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
# -fno-omit-frame-pointer is so perf gets good stack traces
CFLAGS = -ggdb3 -Wall $(OPT)
CXXFLAGS = -std=c++17 -ggdb3 -Wall $(OPT) -fno-omit-frame-pointer
SOFLAGS = -shared -fPIC

OBJS = objects.o translate.o io.o read_cache.o config.o mkcache.o \
	nvme.o write_cache.o file_backend.o \
	rados_backend.o lsvd_debug.o liblsvd.o
CFILES = $(OBJS:.o=.cc)

debug: CXXFLAGS += -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
debug: CXXFLAGS += -Wall -Wextra -Wconversion -Wdouble-promotion -Wno-sign-conversion
release: CXXFLAGS += -O3

debug: liblsvd.so
release: liblsvd.so

all: release imgtool

liblsvd.so:  $(OBJS)
	$(CXX) -std=c++17 $(CFILES) -o liblsvd.so $(OPT) $(CXXFLAGS) $(SOFLAGS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid -lz

%.o: %.d

# CRC_OBJS = crc32.o crc32_simd.o

imgtool: imgtool.o $(OBJS)
	$(CXX) -o $@ imgtool.o $(OBJS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid -lz

lsvd_rnd_test: lsvd_rnd_test.o $(OBJS)
	$(CXX) -o $@ lsvd_rnd_test.o $(OBJS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid -lz

lsvd_crash_test: lsvd_crash_test.o $(OBJS)
	$(CXX) -o $@ lsvd_crash_test.o $(OBJS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid -lz

test-1: test-1.o $(OBJS)
	$(CXX) -o $@ test-1.o $(OBJS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid
test-2: test-2.o $(OBJS)
	$(CXX) -o $@ test-2.o $(OBJS) -lstdc++fs -lpthread -lrados -lrt -laio -luuid

test7: test7.o $(OBJS)
	$(CXX) -o $@ test7.o $(OBJS) $(CRC_OBJS) -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados

test8: test8.o $(OBJS)
	$(CXX) -o $@ test8.o $(OBJS) $(CRC_OBJS) -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados

test9: test9.o $(OBJS)
	$(CXX) -o $@ test9.o $(OBJS) $(CRC_OBJS) -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados

test10: test10.o $(OBJS)
	$(CXX) -o $@ test10.o $(OBJS) $(CRC_OBJS) -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados

# Add .d to Make's recognized suffixes.
SUFFIXES += .d

SOURCES:=$(shell find . -name "*.cc")
#These are the dependency files, which make will clean up after it creates them
DEPFILES:=$(patsubst %.cc,%.d,$(SOURCES))

#This is the rule for creating the dependency files
%.d: %.cc
	$(CXX) $(CXXFLAGS) -MM -MT '$(patsubst %.cc,%.o,$<)' $< -MF $@

sources:
	@echo $(CFILES)

bdus: bdus.o $(OBJS)
	$(CXX) $(OBJS) bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados -laio -luuid

clean:
	rm -f liblsvd.so bdus mkdisk $(OBJS) *.o *.d test7 test8 imgtool

unit-test: unit-test.cc extent.h
	$(CXX) $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	$(CC) $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

-include $(DEPFILES)
