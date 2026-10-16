#pragma once
#include "driver/i2c_master.h"

#include "keys.hpp"

// Keyboard of the Cardputer ADV: TCA8418 controller (0x34) on the internal I2C bus that audio.cpp opened.
// (The espp keyboard driver opens the same I2C port itself and would clash with the codec.)
// A task polls the controller and queues the actions of keys.hpp; the main loop collects them.
bool keyboard_start(i2c_master_bus_handle_t bus);

// Next queued action, or KeyAction::None.
KeyAction keyboard_next();
