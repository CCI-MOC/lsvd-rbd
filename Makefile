CXX = g++
CC = g++

# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
CFLAGS = -ggdb3 -Wall -Wno-psabi $(OPT)
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi -fno-tree-sra $(OPT)
SOFLAGS = -shared -fPIC

OBJS = base_functions.o objects.o translate.o io.o read_cache.o  \
	nvme.o write_cache.o file_backend.o \
	rados_backend.o lsvd_debug.o lsvd.o
CFILES = $(OBJS:.o=.cc)

liblsvd.so:  $(OBJS)
	$(CXX) -std=c++17 $(CFILES) -o liblsvd.so $(OPT) $(CXXFLAGS) $(SOFLAGS) -lstdc++fs -lpthread -lrados -lrt -laio

%.o: %.d

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
	$(CXX) $(OBJS) bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados -laio

clean:
	rm -f liblsvd.so bdus mkdisk $(OBJS) *.o *.d

unit-test: unit-test.cc extent.h
	$(CXX) $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	$(CC) $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

-include $(DEPFILES)
