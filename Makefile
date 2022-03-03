CFLAGS = -ggdb3 -Wall -Wno-psabi
CXXFLAGS = -std=c++17 -ggdb3 -Wall -Wno-psabi
SOFLAGS = -shared -fPIC 

liblsvd.so: first-try.cc extent.cc journal2.cc
	g++ -std=c++17 first-try.cc -o liblsvd.so $(CFLAGS) $(SOFLAGS)

bdus: bdus.o first-try.o extent.cc journal2.cc
	g++ first-try.o bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread

mkdisk: mkdisk.cc objects.cc
	g++ mkdisk.cc -o mkdisk $(CXXFLAGS) -luuid -lstdc++fs

clean:
	rm -f liblsvd.so bdus mkdisk

unit-test: unit-test.cc extent.cc
	g++ $(CXXFLAGS) -o unit-test unit-test.cc
