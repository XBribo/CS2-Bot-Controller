// Shared pawn-service resolution; public registration API stays in InputInjector.h.
#pragma once

namespace cs2bc::input_injector::pawn_binding {
// Initializes shared pawn and weapon resolution.
bool Init();
// Returns validated services for the current registration.
void* ServicesForSlot(int slot);
// Reads the current live pawn without registering or changing a session.
void* CurrentWeaponServices(int slot);
// Reads only the registered pawn; never adopts a replacement.
void* RegisteredWeaponServices(int slot);
// Binds the current pawn for a new input request.
bool BindCurrentPawn(int slot);
// Invalidates changed or disconnected pawns.
void Refresh();
// Ends all pawn sessions without discarding saved data.
void Reset();
// Reads the callback owner; registration is checked separately.
int ServicesToSlot(void* services);
// Resolves weapons only for the registered pawn.
void* ServicesToWeaponServices(int slot, void* services);
// Clears registrations after all pawn-service hooks have drained.
void ClearAll();
} // namespace cs2bc::input_injector::pawn_binding
