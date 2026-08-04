// Cross-platform self-module path

#pragma once

#include <string>

namespace BotController {
// Absolute path of this shared library on disk; empty on failure
std::string SelfModulePath();
} // namespace BotController
