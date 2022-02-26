CFLAGS = -ggdb3 -Wall
CXXFLAGS = -std=c++17 -ggdb3 -Wall
SOFLAGS = -shared -fPIC 

liblsvd.so: first-try.cc extent.cc
	g++ -std=c++17 first-try.cc -o liblsvd.so $(CFLAGS) $(SOFLAGS)

bdus: bdus.o first-try.o
	g++ first-try.o bdus.o -o bdus $(CFLAGS) $(CXXFLAGS) -lbdus -lpthread
