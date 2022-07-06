#include <uuid/uuid.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

#include "objects.h"
#include "argparse.hpp"

#include "scandirpp.h"

size_t parseint(const char *s)
{
    char *p = (char*)s;
    float n = strtof(p, &p);
    switch (*p) {
    case 'k':
    case 'K':
        n = n*1024; break;
    case 'm':
    case 'M':
        n = n*1024*1024; break;
    case 'g':
    case 'G':
        n = n*1024*1024*1024; break;
    }
    return (size_t)n;
}

#include <sys/stat.h>

int main(int argc, const char **argv)
{
    ArgumentParser parser;
    parser.addArgument("-s", "--size", 1);
    parser.addFinalArgument("outfile");
    parser.parse(argc, argv);

    auto outfile = parser.retrieve<std::string>("outfile");
    auto size = parser.retrieve<std::string>("size");
    size_t bytes = parseint(size.c_str());

    fs::path p(outfile);
    auto dir = p.parent_path();
    struct stat sb;
    if (stat(dir.c_str(), &sb) < 0) {
	if (errno == ENOENT)
	    mkdir(dir.c_str(), 0777);
	else {
	    printf("%s: %s\n", dir.c_str(), strerror(errno));
	    exit(1);
	}
    }
    else if (!S_ISDIR(sb.st_mode)) {
	printf("%s: not a directory\n", dir.c_str());
	exit(1);
    }

    auto leaf = p.filename().native() + ".";
    auto filter = [leaf](const std::string& name) {
	return (name.rfind(leaf, 0) == 0);};
    for (const auto& tmp : scandirpp::get_names(dir, filter)) {
	auto f = dir.native() + "/" + tmp;
	if (unlink(f.c_str()) < 0) {
	    printf("%s (remove): %s\n", f.c_str(), strerror(errno));
	    exit(1);
	}
    }

    size_t sectors = bytes / 512;
    uuid_t uuid;
    uuid_generate(uuid);
    
    struct hdr h = {
	.magic = LSVD_MAGIC,
	.version = 1,
	.vol_uuid = {0},
	.type = LSVD_SUPER,
	.seq = 0,
	.hdr_sectors = 8,
	.data_sectors = 0,
    };
    memcpy(h.vol_uuid, uuid, sizeof(uuid));

    struct super_hdr sh = {
	.vol_size = sectors,
	.total_sectors = 0,
	.live_sectors = 0,
	.next_obj = 1,
	.ckpts_offset = 0,
	.ckpts_len = 0,
	.clones_offset = 0,
	.clones_len = 0,
	.snaps_offset = 0,
	.snaps_len = 0
    };

    size_t n_pad = 4096 - sizeof(h) - sizeof(sh);
    char *pad = (char*)calloc(n_pad, 1);

    struct iovec iov[3] = {{(char*)&h, sizeof(h)},
			   {(char*)&sh, sizeof(sh)},
			   {pad, n_pad}};
    int fd = open(outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    writev(fd, iov, 3);
    close(fd);

    return 0;
}
