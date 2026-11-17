#pragma once

class CCommandContext;
class IVEngineServer2;

namespace cs2bc {
namespace commands {
// Caller reply interface; null routes output to the server log.
extern IVEngineServer2* g_engine;

void PrintToCaller(const CCommandContext& context, const char* fmt, ...);
} // namespace commands
} // namespace cs2bc
