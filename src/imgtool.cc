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

FOLLY_INIT_LOGGING_CONFIG(".=DBG,folly=INFO");

using str = std::string;

static usize parseint(str i)
{
    usize processed;
    auto val = std::stoll(i, &processed);
    char *postfix = (char *)i.c_str() + processed;

    if (toupper(*postfix) == 'G')
        val *= (1024 * 1024 * 1024);
    if (toupper(*postfix) == 'M')
        val *= (1024 * 1024);
    if (toupper(*postfix) == 'K')
        val *= 1024;

    return val;
}

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

    fmt::println("Image: {}", name);
    fmt::println("Size (bytes/GB/GiB): {}/{}/{}", sb.image_size,
                 sb.image_size / 1e9, sb.image_size / (1024 * 1024 * 1024));
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

int main(int argc, char **argv)
{
    auto argv_save = argv;
    int i = 0;
    auto folly_init = folly::Init(&i, &argv);
    argv = argv_save;

    namespace po = boost::program_options;
    po::options_description desc("Allowed options");

    // clang-format off
    desc.add_options()
        ("help,h", "produce help message")
        ("cmd,c", po::value<str>(), "subcommand: create, clone, delete, info")
        ("img,i", po::value<str>(), "name of the iname")
        ("pool,p", po::value<str>(), "pool where the image resides")
        ("size,s", po::value<str>()->default_value("1G"), 
                                    "size in bytes (M=2^20,G=2^30)")
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
        auto size = parseint(vm["size"].as<str>());
        auto thick = vm["thick"].as<bool>();
        LsvdImage::create(io, img, size).scheduleOn(exe).start().wait();
    } else if (cmd == "delete") {
        LsvdImage::remove(io, img).scheduleOn(exe).start().wait();
    } else if (cmd == "clone") {
        if (!vm.count("dest"))
            XLOGF(FATAL, "Destination image not specified");
        auto dst = vm["dest"].as<str>();
        LsvdImage::clone(io, img, dst).scheduleOn(exe).start().wait();
    } else if (cmd == "info")
        info(io, img).scheduleOn(exe).start().wait();
    else
        XLOGF(FATAL, "Unknown command '{}'", cmd);
}
