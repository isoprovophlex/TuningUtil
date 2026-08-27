#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <REX/REX.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <windows.h>

#include <format>
#include <rfl/json.hpp>
#include <string>

namespace logger = SKSE::log;
using namespace std::literals;
