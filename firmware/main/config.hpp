#pragma once
#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

// Taken from the usb_device_uac configuration (see sdkconfig.defaults / menuconfig),
// so the waveform code always agrees with what the host is sent.
constexpr uint32_t kUacSampleRate = CONFIG_UAC_SAMPLE_RATE;
constexpr size_t kUacChannels = CONFIG_UAC_SPEAKER_CHANNEL_NUM;  // speaker channels sent by the host
static_assert(CONFIG_UAC_BIT_RESOLUTION == 16, "waveform code assumes 16-bit samples");

constexpr size_t kLcdWidth = 240;
constexpr size_t kLcdHeight = 135;
constexpr size_t kBannerHeight = 24;
constexpr size_t kSamplesPerColumn = 8;
