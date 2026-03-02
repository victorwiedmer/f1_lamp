/*
 * F1LampUsermod registration patch
 * ─────────────────────────────────────────────────────────────────────────────
 * Copy the two blocks below into the WLED source file:
 *   wled00/usermods_list.cpp
 *
 * Do NOT copy this entire file — only the marked blocks.
 * ─────────────────────────────────────────────────────────────────────────────
 */

// ── BLOCK 1 ──────────────────────────────────────────────────────────────────
// Add this #include near the top of usermods_list.cpp,
// alongside the other usermod includes:

#ifdef USERMOD_F1_LAMP
  #include "../usermods/F1LampUsermod/F1LampUsermod.h"
#endif

// ── BLOCK 2 ──────────────────────────────────────────────────────────────────
// Add this line inside the void registerUsermods() function body,
// alongside the other usermods.add() calls:

#ifdef USERMOD_F1_LAMP
  usermods.add(new F1LampUsermod());
#endif
