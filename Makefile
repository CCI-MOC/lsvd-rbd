CFLAGS = -g -Wall
SOFLAGS = -shared -fPIC 

liblsvd.so: first-try.cc extent.cc
	g++ -std=c++17 first-try.cc -o liblsvd.so $(CFLAGS) $(SOFLAGS)
