// license:GPLv3+

#pragma once

#include <cstring>
#include <sstream>
#include <string>
#include <vector>
using namespace std::string_literals;
using namespace std::string_view_literals;
using std::string;

// Shared logging
#include "plugins/LoggingPlugin.h"

namespace Scorbit
{

LPI_USE_CPP();
#define LOGD LPI_LOGD_CPP
#define LOGI LPI_LOGI_CPP
#define LOGW LPI_LOGW_CPP
#define LOGE LPI_LOGE_CPP

}
