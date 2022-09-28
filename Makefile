CXX = g++
CC = g++

# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
CFLAGS = -ggdb3 -Wall -Wno-psabi $(OPT)
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi -fno-tree-sra $(OPT)
SOFLAGS = -shared -fPIC

OBJS = base_functions.o translate.o io.o read_cache.o  \
	nvme.o send_write_request.o write_cache.o file_backend.o \
	rados_backend.o refactor_lsvd.o
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


lsvd_rbd.o: lsvd_rbd.cc read_cache.o
	$(CXX) -c -std=c++17 lsvd_rbd.cc $(OPT) $(CXXFLAGS)


bdus: bdus.o $(OBJS)
	$(CXX) $(OBJS) bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados -laio

mkdisk: mkdisk.cc objects.h
	$(CXX) mkdisk.cc -o mkdisk $(CXXFLAGS) -luuid -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk $(OBJS) *.o *.d

unit-test: unit-test.cc extent.h
	$(CXX) $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	$(CC) $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

check_lsvd_syms: lsvd.py liblsvd.so
	fgrep 'lsvd_lib.' lsvd.py | sed -e 's/.*lsvd_lib.//' -e 's/(.*//' > /tmp/_syms
	nm liblsvd.so | grep U | if fgrep -f /tmp/_syms; then false; else true; fi
	rm -f /tmp/_syms

-include $(DEPFILES)
