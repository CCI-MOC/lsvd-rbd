#pragma once
#include "representation.h"
#include "utils.h"

const auto DEFAULT_JOURNAL_DIR = "/tmp/";

class LsvdConfig
{

  public:
    fstr journal_path;

    static Result<LsvdConfig> parse(fstr imgname, fstr str)
    {
        LsvdConfig cfg{
            .journal_path =
                fmt::format("{}/{}.lsvd_journal", DEFAULT_JOURNAL_DIR, imgname),
        };
        todo();
        return cfg;
    }
};
