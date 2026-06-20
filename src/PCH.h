#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

using namespace std::literals;
