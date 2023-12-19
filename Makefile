CXX = clang++-17
BUILD_DIR = build

.DEFAULT_GOAL := debug
.SILENT: debug release

CFLAGS = -ggdb3 -Wall $(OPT)
CXXFLAGS = -std=c++17 -ggdb3 $(OPT) -fno-omit-frame-pointer -fPIC
LDFLAGS = -lstdc++fs -lpthread -lrt -laio -luuid -lz -lrados -lfmt -luring
LDFLAGS += -fuse-ld=mold
SOFLAGS = -shared -fPIC

TARGET_EXECS = imgtool thick-image
TEST_EXECS = lsvd_crash_test lsvd_rnd_test test-rados test-seq unit-test 
LSVD_DEPS = objects.o translate.o io.o img_reader.o config.o mkcache.o \
	nvme.o write_cache.o file_backend.o shared_read_cache.o \
	rados_backend.o lsvd_debug.o liblsvd.o

debug: CXXFLAGS += -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
debug: CXXFLAGS += -fsanitize=address
# debug: CXXFLAGS += -fsanitize=thread
debug: CXXFLAGS += -Wall -Wextra -Wdouble-promotion -Wno-sign-conversion -Wno-conversion -Wno-unused-parameter
debug: CXXFLAGS += -O0 -fno-inline
nosan: CXXFLAGS += -O0 -fno-inline
release: CXXFLAGS += -O3 -DLOGLV=1

debug: liblsvd.so imgtool thick-image lsvd_rnd_test test-seq
nosan: liblsvd.so imgtool thick-image
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

$(TARGET_EXECS): %: $(BUILD_DIR)/%.o $(LSVD_OBJS)
	@echo "LD $@"
	@$(CXX) -o $@ $< $(LSVD_OBJS) $(CXXFLAGS) $(LDFLAGS)

$(TEST_EXECS): %: $(BUILD_DIR)/test/%.o $(LSVD_OBJS)
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
	@rm -f liblsvd.so $(OBJS) $(DEPS) *.o *.d $(TARGET_EXECS)

install-deps:
	sudo apt install libfmt-dev libaio-dev librados-dev mold
	# echo "You'll have to compile the latest version of liburing yourself"

