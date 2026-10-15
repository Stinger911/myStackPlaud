#pragma once
#include <cstdint>

// Coarse progress marker written by the main loop and by screen_draw(); read by the
// watchdog task to report where the main loop got stuck.
extern volatile uint32_t g_dbg_stage;
