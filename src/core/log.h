#pragma once

#include "core/cs2_sdk/src/core/log.h"

namespace cs2bc {
namespace log = cs2_sdk::log;
}

#define BC_LOG_DEBUG(...) ::cs2bc::log::Write(::cs2bc::log::Level::Debug, __VA_ARGS__)
#define BC_LOG_INFO(...)  ::cs2bc::log::Write(::cs2bc::log::Level::Info, __VA_ARGS__)
#define BC_LOG_WARN(...)  ::cs2bc::log::Write(::cs2bc::log::Level::Warn, __VA_ARGS__)
#define BC_LOG_ERROR(...) ::cs2bc::log::Write(::cs2bc::log::Level::Error, __VA_ARGS__)
