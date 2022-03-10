CFLAGS = -ggdb3 -Wall -Wno-psabi
# no-psabi - disable warnings for changes in aa64 ABI
# no-tree-sra - tree-sra causes unit-test to segfault on x86-64, not on aa64
# note that no-unswitch-loops also gets rid of the segfault
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi -fno-tree-sra
SOFLAGS = -shared -fPIC

liblsvd.so: first-try.cc extent.cc journal2.cc
	g++ -std=c++17 first-try.cc -o liblsvd.so $(OPT) $(CXXFLAGS) $(SOFLAGS) -lstdc++fs

bdus: bdus.o first-try.o extent.cc journal2.cc
	g++ first-try.o bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread -lstdc++fs

mkdisk: mkdisk.cc objects.cc
	g++ mkdisk.cc -o mkdisk $(CXXFLAGS) -luuid -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk

unit-test: unit-test.cc extent.cc
	g++ $(OPT) $(CXXFLAGS) -o unit-test unit-test.cc -lstdc++fs

unit-test-O3: unit-test.cc extent.cc
	g++ $(CXXFLAGS) -O3 -o $@ unit-test.cc -lstdc++fs

check_lsvd_syms: lsvd.py liblsvd.so
	fgrep 'lsvd_lib.' lsvd.py | sed -e 's/.*lsvd_lib.//' -e 's/(.*//' > /tmp/_syms
	nm liblsvd.so | grep U | if fgrep -f /tmp/_syms; then false; else true; fi
	rm -f /tmp/_syms
