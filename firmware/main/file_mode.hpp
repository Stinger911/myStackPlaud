#pragma once
#include <cstdint>

// File mode (specs/M4-device-ux.md, R4.5 / R4.10): the SD card is exposed to the computer as a USB flash
// drive. The USB stack is not switched at run time: `F` stores a flag in RTC memory and restarts the
// device, and the next boot brings up the mass-storage descriptor instead of the audio one (one USB-C
// port, so the two are exclusive anyway). Leaving works the same way: `F` or an eject on the computer
// restarts into the normal mode. The recorder is not started in file mode, so nothing else touches the card.

// Ask for file mode on the next boot and restart. Does not return.
[[noreturn]] void file_mode_enter();

// Leave file mode: the next boot is a normal one. Does not return.
[[noreturn]] void file_mode_leave();

// True once after a restart requested by file_mode_enter(); the flag is consumed.
bool file_mode_boot_requested();

enum class FileModeState : uint8_t { Ok, NoCard, UsbFailed };

// Bring up the card (raw sectors, no FAT) and the USB mass-storage device.
FileModeState file_mode_start();

// The computer ejected the disk.
bool file_mode_ejected();

// Card capacity in 512-byte sectors, 0 without a card.
uint32_t file_mode_sectors();
