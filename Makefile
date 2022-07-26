CFLAGS = -ggdb3 -Wall -Wno-psabi
CC = g++
# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi -fno-tree-sra
SOFLAGS = -shared -fPIC
OBJS = misc_cache.o translate.o io.o disk.o read_cache.o write_cache.o file_backend.o rados_backend.o refactor_lsvd.o
CFILES = misc_cache.cc translate.cc io.cc disk.cc read_cache.cc write_cache.cc file_backend.cc rados_backend.cc refactor_lsvd.cc

# liblsvd.so: lsvd_rbd.cc extent.h journal2.h objects.h
liblsvd.so:  $(OBJS)
	$(CC) -std=c++17 $(CFILES) -o liblsvd.so $(OPT) $(CXXFLAGS) $(SOFLAGS) -lstdc++fs -lpthread -lrados -lrt -laio


%.o: %.d

# %.o : %.cc
# 	$(CC) $(CXXFLAGS) -o $@ -c $^

# Add .d to Make's recognized suffixes.
SUFFIXES += .d

SOURCES:=$(shell find . -name "*.cc")
#These are the dependency files, which make will clean up after it creates them
DEPFILES:=$(patsubst %.cc,%.d,$(SOURCES))


#This is the rule for creating the dependency files
%.d: %.cc
	$(CXX) $(CXXFLAGS) -MM -MT '$(patsubst %.cc,%.o,$<)' $< -MF $@


lsvd_rbd.o: lsvd_rbd.cc read_cache.o
	$(CC) -c -std=c++17 lsvd_rbd.cc $(OPT) $(CXXFLAGS)


bdus: bdus.o lsvd_rbd.o extent.h journal2.h
	$(CC) lsvd_rbd.o bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs -lrados

mkdisk: mkdisk.cc objects.h
	$(CC) mkdisk.cc -o mkdisk $(CXXFLAGS) -luuid -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk $(OBJS) *.o *.d

unit-test: unit-test.cc extent.h
	$(CC) $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.h
	$(CC) $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

check_lsvd_syms: lsvd.py liblsvd.so
	fgrep 'lsvd_lib.' lsvd.py | sed -e 's/.*lsvd_lib.//' -e 's/(.*//' > /tmp/_syms
	nm liblsvd.so | grep U | if fgrep -f /tmp/_syms; then false; else true; fi
	rm -f /tmp/_syms

-include $(DEPFILES)
