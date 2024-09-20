#include "cachelib/allocator/CacheAllocator.h"
#include "image.h"
#include "representation.h"
#include "utils.h"
#include "zpp_bits.h"

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789+/";

std::string base64_encode(const char *bytes_to_encode, unsigned int in_len)
{
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) +
                              ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) +
                              ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++)
                ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] =
            ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] =
            ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; (j < i + 1); j++)
            ret += base64_chars[char_array_4[j]];

        while ((i++ < 3))
            ret += '=';
    }

    return ret;
}

int main(int argc, char **argv)
{

    vec<byte> buf;
    zpp::bits::out ar(buf);

    SuperblockInfo sb;
    sb.clones = {{1, "a"}, {2, "b"}};
    std::ignore = ar(sb);
    std::cout << base64_encode((const char *)buf.data(), buf.size())
              << std::endl;

    using Cache = facebook::cachelib::LruAllocator;
    Cache::Config cachecfg;
    cachecfg.setCacheSize(1 * 1024 * 1024 * 1024)
        .setCacheName("My cache")
        .validate();

    uptr<Cache> cache = std::make_unique<Cache>(cachecfg);
    facebook::cachelib::PoolId pid =
        cache->addPool("default", cache->getCacheMemoryStats().ramCacheSize);

    {
        auto whandle = cache->allocate(pid, "key1", buf.size());
        std::memcpy(whandle->getMemory(), buf.data(), buf.size());
        cache->insert(whandle);
    }

    vec<byte> buf2;
    {
        auto rhandle = cache->find("key1");
        buf2.resize(rhandle->getSize());
        std::memcpy(buf2.data(), rhandle->getMemory(), rhandle->getSize());
    }

    std::cout << base64_encode((const char *)buf2.data(), buf2.size())
              << std::endl;

    return 0;
}