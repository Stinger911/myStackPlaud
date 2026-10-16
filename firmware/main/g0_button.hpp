#pragma once
#include <algorithm>
#include <cstdint>

// G0 button logic (specs/M4-device-ux.md, R4.7): a short press starts a recording, stopping needs
// a 1 s hold so a touch in a pocket cannot end a meeting. A released-early hold, or a short press
// while recording, raises a hint ("HOLD TO STOP") for a moment. Pure logic, no hardware access:
// call update() every frame with the current pin state.

enum class G0Context : uint8_t {
    Idle,       // not recording: a press starts
    Recording,  // recording: a hold stops
    Busy,       // starting or stopping: presses are ignored
};

enum class G0Event : uint8_t { None, Start, Stop };

struct G0Output {
    G0Event event = G0Event::None;
    bool holding = false;       // a stop hold is in progress (draw the ring)
    float hold_progress = 0.0f;  // 0..1
    bool hint = false;           // show "HOLD TO STOP"
};

class G0Button {
public:
    static constexpr uint32_t kHoldMs = 1000;
    static constexpr uint32_t kHintMs = 1500;

    G0Output update(bool pressed, G0Context ctx, uint32_t now_ms) {
        G0Output out;
        if (pressed && !was_pressed_) {  // press edge: decide what this press is for
            press_ms_ = now_ms;
            if (ctx == G0Context::Idle) {
                action_ = Action::Done;  // acts at once, then the press is used up
                out.event = G0Event::Start;
            } else if (ctx == G0Context::Recording) {
                action_ = Action::Hold;
            } else {
                action_ = Action::Done;
            }
        }
        if (pressed && action_ == Action::Hold) {
            if (ctx != G0Context::Recording) {
                action_ = Action::Done;  // the recorder stopped by itself (error): nothing left to hold for
            } else {
                uint32_t held = now_ms - press_ms_;
                out.holding = true;
                out.hold_progress = std::min(1.0f, static_cast<float>(held) / kHoldMs);
                if (held >= kHoldMs) {
                    out.event = G0Event::Stop;
                    action_ = Action::Done;
                    out.holding = false;
                }
            }
        }
        if (!pressed && was_pressed_ && action_ == Action::Hold) {  // released early
            hint_until_ms_ = now_ms + kHintMs;
            hint_armed_ = true;
        }
        if (!pressed) action_ = Action::None;
        was_pressed_ = pressed;

        if (hint_armed_ && static_cast<int32_t>(hint_until_ms_ - now_ms) > 0 && ctx == G0Context::Recording)
            out.hint = true;
        else
            hint_armed_ = false;
        return out;
    }

private:
    enum class Action : uint8_t { None, Hold, Done };
    bool was_pressed_ = false;
    Action action_ = Action::None;
    uint32_t press_ms_ = 0;
    uint32_t hint_until_ms_ = 0;
    bool hint_armed_ = false;
};
