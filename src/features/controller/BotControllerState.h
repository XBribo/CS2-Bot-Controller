// Per-slot bot lock flags.

#pragma once

namespace cs2bc {
namespace bot_controller_state {
constexpr int kMaxSlots = 64;

// Lets idle view hooks skip resolving native bot/pawn/controller pointers.
bool HasAnyLock();

// All lock: Update + Upkeep.
bool GetAll(int slot);
void SetAll(int slot, bool locked);
void ClearAllAll();
int CountAll();

// Aim lock: Upkeep only.
bool GetAim(int slot);
void SetAim(int slot, bool locked);
void ClearAllAim();
int CountAim();
} // namespace bot_controller_state
} // namespace cs2bc
