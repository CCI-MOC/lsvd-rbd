#include "folly/String.h"
#include "folly/executors/GlobalExecutor.h"
#include <argp.h>
#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <cstdlib>
#include <fcntl.h>
#include <fmt/format.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <rados/librados.h>
#include <stdlib.h>
#include <string>

#include "image.h"
#include "representation.h"

FOLLY_INIT_LOGGING_CONFIG(".=INFO,folly=INFO");

using str = std::string;

static auto info(sptr<ObjStore> s3, str name) -> Task<void>
{
    XLOGF(INFO, "Getting info for image {}", name);
    SuperblockInfo sb;
    auto sbuf = co_await s3->read_all(name);
    if (sbuf.has_failure()) {
        XLOGF(ERR, "Failed to read superblock: {}", sbuf.error().message());
        co_return;
    }

    (sb.deserialise(sbuf.value())).value();

    auto size_bytes = sb.image_size;
    auto size_gb = sb.image_size / 1e9;
    auto size_gib = sb.image_size / (1024 * 1024 * 1024);

    fmt::println("Image: {}", name);
    fmt::println("Size: {} GiB, {} GB, {} bytes", size_gib, size_gb,
                 size_bytes);
    fmt::println("Checkpoints: [{}]", folly::join(", ", sb.checkpoints));
    fmt::println("Snapshots: [{}]", folly::join(", ", sb.snapshots));

    for (auto &[a, b] : sb.clones)
        fmt::println("Clone: {} -> {}", a, b);

    // look at the extmap
    auto last_ckpt = sb.checkpoints.back();
    auto key = get_logobj_key(name, last_ckpt);
    auto extmap = ExtMap::deserialise((co_await s3->read_all(key)).value());
    auto extmap_s = co_await extmap->to_string();
    fmt::print("Extent map:\n{}", extmap_s);
}

static auto objinfo(sptr<ObjStore> s3, str name, usize seq) -> Task<void>
{
    auto key = get_logobj_key(name, seq);
    XLOGF(INFO, "Analysing object {}", key);
    auto obj_bytes = co_await s3->get_size(key);
    if (obj_bytes.has_failure()) {
        XLOGF(ERR, "Failed to stat object: {}", obj_bytes.error().message());
        co_return;
    }

    vec<byte> buf(obj_bytes.value());
    auto fetch_res = co_await s3->read(key, 0, iovec{buf.data(), buf.size()});
    if (fetch_res.has_failure()) {
        XLOGF(ERR, "Failed to read object: {}", fetch_res.error().message());
        co_return;
    }

    fmt::println("Object: {}, size: {}\nHexdump:\n{}", key, buf.size(),
                 folly::hexDump(buf.data(), buf.size()));

    fmt::println("Deserialising log");
    u32 consumed_bytes = 0;
    while (consumed_bytes < buf.size()) {
        auto entry =
            reinterpret_cast<log_entry_hdr_only *>(buf.data() + consumed_bytes);
        switch (entry->type) {
        case log_entry_type::WRITE:
            fmt::println("WRITE: off={:#x} len={}", (u32)entry->offset,
                         entry->len);
            consumed_bytes += sizeof(log_entry) + entry->len;
            break;
        case log_entry_type::TRIM:
            fmt::println("TRIM: off={:#x} len={}", (u32)entry->offset,
                         entry->len);
            consumed_bytes += sizeof(log_entry);
            break;
        default:
            XLOGF(ERR, "Unknown log entry type {}", (u32)(entry->type));
        }
    }
}

int main(int argc, char **argv)
{
    int fake_argc = 1;
    auto folly_init = folly::Init(&fake_argc, &argv, false);

    namespace po = boost::program_options;
    po::options_description desc("Allowed options");

    // clang-format off
    desc.add_options()
        ("help,h", "produce help message")
        ("cmd,c", po::value<str>(), "subcommand: create, clone, delete, info, objinfo")
        ("img,i", po::value<str>(), "name of the iname")
        ("pool,p", po::value<str>(), "pool where the image resides")
        ("size,s", po::value<str>()->default_value("1G"), 
                                    "size in bytes (M=2^20,G=2^30)")
        ("seq,q", po::value<usize>(), "seqnum for objinfo")
        ("thick", po::value<bool>()->default_value(false), 
            "thick provision when creating an image (not currently supported)")
        ("dest", po::value<str>(), "destination (for clone)");
    // clang-format on

    po::positional_options_description p;
    p.add("cmd", 1).add("pool", 1).add("img", 1);

    po::variables_map vm;
    po::store(
        po::command_line_parser(argc, argv).options(desc).positional(p).run(),
        vm);
    po::notify(vm);

    if (vm.count("help") || !vm.count("cmd") || !vm.count("pool") ||
        !vm.count("img")) {
        std::cout << desc << "\n";
        return 1;
    }

    auto exe = folly::getGlobalCPUExecutor();

    auto cmd = vm["cmd"].as<str>();
    auto pool = vm["pool"].as<str>();
    auto img = vm["img"].as<str>();

    XLOGF(DBG, "Running command {}", cmd);
    auto io = ObjStore::connect_to_pool(pool).value();

    if (cmd == "create") {
        auto size = parse_size_str(vm["size"].as<str>());
        auto thick = vm["thick"].as<bool>();
        XLOGF(INFO, "Creating '{}/{}', size={} bytes", pool, img, size);
        LsvdImage::create(io, img, size).scheduleOn(exe).start().wait();
    } else if (cmd == "delete") {
        XLOGF(INFO, "Deleting '{}/{}'", pool, img);
        LsvdImage::remove(io, img).scheduleOn(exe).start().wait();
    } else if (cmd == "clone") {
        if (!vm.count("dest"))
            XLOGF(FATAL, "Destination image not specified");
        auto dst = vm["dest"].as<str>();
        XLOGF(INFO, "Cloning '{}/{}' to '{}/{}'", pool, img, pool, dst);
        LsvdImage::clone(io, img, dst).scheduleOn(exe).start().wait();
    } else if (cmd == "info") {
        XLOGF(INFO, "Getting info for '{}/{}'", pool, img);
        info(io, img).scheduleOn(exe).start().wait();
    } else if (cmd == "objinfo") {
        auto seq = vm["seq"].as<usize>();
        XLOGF(INFO, "Analysing '{}/{}' seq {:#x}", pool, img, seq);
        objinfo(io, img, seq).scheduleOn(exe).start().wait();
    } else
        XLOGF(FATAL, "Unknown command '{}'", cmd);
}
