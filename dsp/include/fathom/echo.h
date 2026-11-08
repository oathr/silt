#pragma once

#include <array>

#include "absorption.h"
#include "common.h"

namespace fathom {

// A distinct echo: a wall at DISTANCE (a cliff, a reef face, a hull, an ice shelf) sends
// the sound back after 2 d / c, c = 1500 m/s (75 m -> 100 ms; in air it would take 437 ms).
// Each return is the wall's reflection coefficient times the treble the wall scatters
// (roughness, as for the sea floor), the absorption of the round trip, and MUFFLE.
// FEEDBACK = the sound bouncing back and forth between the wall and the listener's side.
// Returns alternate left / right (a wall is never exactly in front).
class Echo {
public:
    struct Wall {
        const char* name;
        float reflection, roughness;  // pressure coefficient, rms metres
        float ring, ringHz;           // structural ringing (a steel hull), Hz
    };
    static constexpr int kWalls = 4;
    static const std::array<Wall, kWalls>& walls() {
        static const std::array<Wall, kWalls> list = {{
            {"Cliff", 0.70f, 0.12f, 0.0f, 0.0f},
            {"Reef", 0.45f, 0.35f, 0.0f, 0.0f},
            {"Hull", 0.92f, 0.01f, 1.0f, 420.0f},
            {"Ice", 0.85f, 0.04f, 0.0f, 0.0f},
        }};
        return list;
    }
    struct Settings {
        float distanceM = 60.0f, feedback = 0.3f, width = 1.0f, muffleHz = 0.0f, waves = 0.0f;
        int wall = 0;
    };

    void prepare(float sampleRate) {
        fs_ = sampleRate;
        for (auto& l : lines_) l.prepare(int(4.2f * sampleRate));
        mod_.seed(0xEC40u);
        dirty_ = true;
        first_ = true;
        reset();
    }
    void reset() {
        for (auto& l : lines_) l.clear();
        for (auto* bank : {&lp1_, &lp2_, &hp_, &ring_})
            for (auto& f : *bank) f.reset();
        first_ = true;
    }
    void set(const Settings& s) {
        if (s.distanceM != s_.distanceM || s.feedback != s_.feedback || s.wall != s_.wall || s.muffleHz != s_.muffleHz || s.width != s_.width) dirty_ = true;
        s_ = s;
    }

    void update(int blockSamples) {
        const float target = std::min(2.0f * s_.distanceM / kWaterC * fs_, float(lines_[0].maxDelay() - 16));
        if (first_) delay_ = target;
        // A moving wall distance glides (a pitch bend, like a real moving reflector).
        delay_ += (target - delay_) * (first_ ? 1.0f : smoothingCoef(0.25f, fs_, blockSamples));
        wobble_ = s_.waves * 0.0004f * fs_ * mod_.advance(float(blockSamples) / fs_, 0.15f);
        first_ = false;
        if (!dirty_) return;
        dirty_ = false;
        const Wall& w = walls()[size_t(std::clamp(s_.wall, 0, kWalls - 1))];
        // Normal incidence: scattering corner 99 / h Hz; absorption over the round trip.
        float corner = std::min(20000.0f, 99.3f / std::max(w.roughness, 1.0e-3f));
        const float absorption = absorptionCornerHz(2.0f * s_.distanceM, 1.0f, 20.0f);
        if (absorption > 0.0f) corner = std::min(corner, absorption);
        if (s_.muffleHz > 0.0f) corner = std::min(corner, s_.muffleHz);
        for (int ch = 0; ch < 2; ++ch) {
            lp1_[size_t(ch)].set(Svf::Type::Lowpass, corner, 0.6f, 0.0f, fs_);
            lp2_[size_t(ch)].set(Svf::Type::Lowpass, std::min(corner * 1.6f, 20000.0f), 0.55f, 0.0f, fs_);
            hp_[size_t(ch)].set(Svf::Type::Highpass, 45.0f, 0.6f, 0.0f, fs_);
            ring_[size_t(ch)].set(Svf::Type::Bell, w.ringHz > 0.0f ? w.ringHz * (ch ? 1.07f : 1.0f) : 1000.0f, 9.0f, 9.0f * w.ring, fs_);
        }
        ringing_ = w.ring > 0.0f;
        reflection_ = w.reflection;
        feedback_ = std::clamp(s_.feedback, 0.0f, 0.95f);
        width_ = s_.width;
    }

    // Returns the echoes only.
    void process(float inL, float inR, float& outL, float& outR) {
        const float d = std::max(2.0f, delay_ + wobble_);
        float a = lines_[0].read(d), b = lines_[1].read(d);
        a = hp_[0].process(lp2_[0].process(lp1_[0].process(a)));
        b = hp_[1].process(lp2_[1].process(lp1_[1].process(b)));
        if (ringing_) a = ring_[0].process(a), b = ring_[1].process(b);
        // Ping-pong: what came back on the left goes out again on the right.
        const float mono = 0.5f * (inL + inR);
        const float sideL = inL + (mono - inL) * (1.0f - width_), sideR = inR + (mono - inR) * (1.0f - width_);
        lines_[0].write(reflection_ * sideL + feedback_ * (width_ * b + (1.0f - width_) * a));
        lines_[1].write(reflection_ * sideR + feedback_ * (width_ * a + (1.0f - width_) * b));
        outL = a, outR = b;
    }

    float delayMs() const { return delay_ / fs_ * 1000.0f; }

private:
    float fs_ = 48000.0f;
    Settings s_;
    bool dirty_ = true, first_ = true, ringing_ = false;
    std::array<DelayLine, 2> lines_;
    std::array<Svf, 2> lp1_, lp2_, hp_, ring_;
    Drift mod_;
    float delay_ = 1000.0f, wobble_ = 0.0f, reflection_ = 0.7f, feedback_ = 0.3f, width_ = 1.0f;
};

}  // namespace fathom
