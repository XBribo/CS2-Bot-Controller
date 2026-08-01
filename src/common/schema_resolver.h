// Runtime SchemaSystem field-offset resolver

#pragma once

#include <cstddef>

namespace BotController::Schema {

// Resolves the live SchemaSystem interface and server type scope
bool Init(char* errorOut, size_t errorOutLen);

// Returns one declared field offset, or -1 when it cannot be resolved
int GetFieldOffset(const char* className, const char* fieldName);

// Clears the cached interface, type scope, and field offsets
void Reset();

} // namespace BotController::Schema
