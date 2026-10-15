#pragma once
#include <cstddef>
#include <cstdint>

// Must match the usb_device_uac configuration (idf.py menuconfig).
// Values here are assumptions until the first run on hardware.
constexpr uint32_t kUacSampleRate = 48000;
constexpr size_t kUacChannels = 2;  // speaker channels sent by the host

constexpr size_t kLcdWidth = 240;
constexpr size_t kLcdHeight = 135;
constexpr size_t kBannerHeight = 24;
constexpr size_t kSamplesPerColumn = 8;
