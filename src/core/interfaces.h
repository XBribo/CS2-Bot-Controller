#pragma once
#include <ISmmAPI.h>
#include <cstddef>

class ISchemaSystem;
extern ISchemaSystem* g_schemaSystem;

namespace cs2bc::interfaces {
// Resolves required interfaces and configures optional voice support.
bool Init(SourceMM::ISmmAPI* ismm, char* error, size_t maxlen);
// Returns the fixed CS2 simulation interval used to rebase movement history.
float TickInterval();
// Clears consumers of the borrowed engine interfaces.
void Reset();
} // namespace cs2bc::interfaces
