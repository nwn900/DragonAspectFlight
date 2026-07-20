#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

using namespace std::literals;
