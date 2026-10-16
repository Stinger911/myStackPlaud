#pragma once

// Settings kept in NVS (flash) across reboots.
void settings_init();
int settings_volume(int fallback);
void settings_set_volume(int level);
