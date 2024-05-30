#include <argp.h>
#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <cstdlib>
#include <fcntl.h>
#include <fmt/format.h>
#include <rados/librados.h>
#include <stdlib.h>
#include <string>
#include <uuid/uuid.h>

#include "backend.h"
#include "image.h"
#include "objects.h"
#include "utils.h"

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

static void create(rados_ioctx_t io, str name, usize size)
{
    auto rc = lsvd_image::create_new(name, size, io);
    THROW_MSG_ON(rc != 0, "Failed to create new image '{}'", name);
}

static void remove(rados_ioctx_t io, str name)
{
    auto rc = lsvd_image::delete_image(name, io);
    THROW_MSG_ON(rc != 0, "Failed to delete image '{}'", name);
}

static void clone(rados_ioctx_t io, str src, str dst)
{
    auto rc = lsvd_image::clone_image(src, dst, io);
    THROW_MSG_ON(rc != 0, "Failed to clone image '{}' to '{}'", src, dst);
}

static void info(rados_ioctx_t io, str name)
{
    auto be = make_rados_backend(io);
    auto parser = object_reader(be);

    auto sb = parser.read_superblock(name);
    THROW_MSG_ON(!sb, "Superblock not found");

    auto i = *sb;
    char uuid_str[37];
    uuid_unparse_lower(i.uuid, uuid_str);

    using namespace fmt;
    print("=== Image info ===\n");
    print("Name: {}\n", name);
    print("UUID: {}\n", uuid_str);
    print("Size: {} bytes / {} GiB\n", i.vol_size,
          (double)i.vol_size / 1024 / 1024 / 1024);
    print("Checkpoints: {}\n", i.ckpts);

    for (auto &c : i.clones)
        print("Base: '{}' upto seq {}\n", c->name, c->last_seq);

    for (auto &c : i.snaps)
        print("Snapshot: '{}' at seq {}\n", c->name, c->seq);
}

int main(int argc, char **argv)
{
    std::set_terminate([]() {
        try {
            std::cerr << boost::stacktrace::stacktrace();
        } catch (...) {
        }
        std::abort();
    });

    namespace po = boost::program_options;
    po::options_description desc("Allowed options");

    // clang-format off
    desc.add_options()
        ("help", "produce help message")
        ("cmd", po::value<str>(), "subcommand: create, clone, delete, info")
        ("img", po::value<str>(), "name of the iname")
        ("pool", po::value<str>(), "pool where the image resides")
        ("size", po::value<str>()->default_value("1G"), 
                                    "size in bytes (M=2^20,G=2^30)")
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

    auto cmd = vm["cmd"].as<str>();
    auto pool = vm["pool"].as<str>();
    auto img = vm["img"].as<str>();

    auto io = connect_to_pool(pool);
    THROW_MSG_ON(io == nullptr, "Failed to connect to pool '{}'", pool);

    if (cmd == "create") {
        auto size = parseint(vm["size"].as<str>());
        create(io, img, size);
    } else if (cmd == "delete")
        remove(io, img);
    else if (cmd == "clone") {
        THROW_MSG_ON(!vm.count("dest"), "Destination image not specified");
        auto dst = vm["dest"].as<str>();
        clone(io, img, dst);
    } else if (cmd == "info")
        info(io, img);
    else
        THROW_MSG_ON(true, "Unknown command '{}'", cmd);

    rados_ioctx_destroy(io);
}
