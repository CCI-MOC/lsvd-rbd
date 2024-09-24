#pragma once
#include "representation.h"
#include "utils.h"

class LsvdConfig
{
  public:
    fstr journal_path;

    static Result<LsvdConfig> parse(fstr str);
};
