CXX = clang++-17
#CXX = g++-12
BUILD_DIR = build

.DEFAULT_GOAL := debug
.SILENT: debug release

CXXFLAGS = -std=c++20 -ggdb3 -fno-omit-frame-pointer -fPIC -I./liburing/src/include
CXXFLAGS += -fsized-deallocation
LDFLAGS = -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados -lfmt -ltcmalloc
LDFLAGS += -fuse-ld=mold -L./liburing/src -l:liburing.a
SOFLAGS = -shared -fPIC

TARGET_EXECS = imgtool thick-image
TEST_EXECS = lsvd_crash_test lsvd_rnd_test test-rados test-seq unit-test 
LSVD_DEPS = objects.o translate.o io.o img_reader.o config.o mkcache.o \
	nvme.o write_cache.o file_backend.o shared_read_cache.o \
	rados_backend.o lsvd_debug.o liblsvd.o image.o

debug: CXXFLAGS += -fsanitize=undefined -fsanitize=float-divide-by-zero -fsanitize=local-bounds 
debug: CXXFLAGS += -fsanitize=implicit-conversion -fsanitize=nullability -fsanitize=integer
debug: CXXFLAGS += -fsanitize=address
# debug: CXXFLAGS += -fsanitize=thread
debug: CXXFLAGS += -Wall -Wextra -Wdouble-promotion -Wno-sign-conversion -Wno-conversion -Wno-unused-parameter
debug: CXXFLAGS += -O0 -fno-inline -DLOGLV=1
nosan: CXXFLAGS += -Og -fno-inline
release: CXXFLAGS += -O3 -DLOGLV=1

debug: liblsvd.so imgtool thick-image test-seq
nosan: liblsvd.so imgtool thick-image test-seq
release: liblsvd.so imgtool thick-image

CPP = $(wildcard *.cc)
OBJS = $(CPP:%.cc=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

$(BUILD_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CXX) -MMD -MP -o $@ -c $< $(CXXFLAGS)

LSVD_OBJS = $(LSVD_DEPS:%.o=$(BUILD_DIR)/%.o)

include $(wildcard $(BUILD_DIR)/*.d)

$(TARGET_EXECS): %: $(BUILD_DIR)/%.o $(LSVD_OBJS) liblsvd.so
	@echo "LD $@"
	@$(CXX) -o $@ $< $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

$(TEST_EXECS): %: $(BUILD_DIR)/test/%.o $(LSVD_OBJS) liblsvd.so
	@echo "LD $@"
	@$(CXX) -o $@ $< $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

$(BUILD_DIR)/test/%.o: test/%.cc
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CXX) -MMD -MP -o $@ -c $< $(CXXFLAGS) -I.

liblsvd.so: $(LSVD_OBJS)
	@echo "LD $@"
	@$(CXX) $(SOFLAGS) -o $@ $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

clean:
	@rm -rf liblsvd.so $(TARGET_EXECS) $(TEST_EXECS) $(BUILD_DIR)/*

install-deps:
	sudo apt install libfmt-dev libaio-dev librados-dev mold
	# echo "You'll have to compile the latest version of liburing yourself"

