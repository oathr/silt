#pragma once

#include <array>

#include "common.h"

namespace silt {

// MUFFLE: how much the water swallows the treble. At 0 the water is exactly physical
// (Francois-Garrison absorption over the real distance, almost nothing at scene range);
// turning it up moves a steep low-pass (24 dB/oct, the f² law of absorption pushed to its
// end) down to 90 Hz, and lets a soft resonance gather just under the corner: the
// pressure-in-the-ears boom of a film plunge. It is the knob to abuse.
inline float muffleCornerHz(float t) {
    if (t <= 0.0f) return 0.0f;
    return 20000.0f * std::pow(90.0f / 20000.0f, std::pow(t, 0.75f));
}

class Muffle {
public:
    void prepare(float sampleRate) { fs_ = sampleRate, reset(); }
    void reset() {
        for (auto* bank : {&a_, &b_, &boom_})
            for (auto& f : *bank) f.reset();
    }
    // corner 0 = off; amount 0..1 scales the boom.
    void set(float corner, float amount) {
        on_ = corner > 0.0f && corner < 19000.0f;
        if (!on_) return;
        if (corner == corner_ && amount == amount_) return;
        corner_ = corner, amount_ = amount;
        for (int ch = 0; ch < 2; ++ch) {
            a_[size_t(ch)].set(Svf::Type::Lowpass, corner, 0.54f, 0.0f, fs_);
            b_[size_t(ch)].set(Svf::Type::Lowpass, corner, 1.0f + 0.6f * amount, 0.0f, fs_);
            boom_[size_t(ch)].set(Svf::Type::Bell, std::max(40.0f, corner * 0.55f), 0.9f, 4.0f * amount, fs_);
        }
        // Level match, as in the ear: pink-noise power through the three filters, weighted
        // to the music band, brought back towards 0 dB (at most +12): on a master, muffling
        // changes the colour, not the loudness.
        float num = 0.0f, den = 0.0f;
        for (int k = 0; k < 48; ++k) {
            const float hz = 31.5f * std::pow(2.0f, float(k) * 9.0f / 47.0f);
            const float w = hz < 100.0f ? hz / 100.0f : hz > 6000.0f ? 6000.0f / hz : 1.0f;
            const float mag = a_[0].magnitude(hz, fs_) * b_[0].magnitude(hz, fs_) * boom_[0].magnitude(hz, fs_);
            num += w * mag * mag, den += w;
        }
        makeup_ = std::min(dbToGain(12.0f), 1.0f / std::sqrt(std::max(num / den, 1.0e-6f)));
    }
    void process(float& l, float& r) {
        if (!on_) return;
        l = makeup_ * boom_[0].process(b_[0].process(a_[0].process(l)));
        r = makeup_ * boom_[1].process(b_[1].process(a_[1].process(r)));
    }
    bool on() const { return on_; }

private:
    float fs_ = 48000.0f, corner_ = -1.0f, amount_ = -1.0f, makeup_ = 1.0f;
    bool on_ = false;
    std::array<Svf, 2> a_, b_, boom_;
};

// The EQ on the wet: low cut, low shelf, one sweepable bell, high shelf. Flat = no cost.
class Eq {
public:
    struct Settings {
        float lowCutHz = 0.0f, lowDb = 0.0f, midHz = 800.0f, midDb = 0.0f, highDb = 0.0f;
    };
    void prepare(float sampleRate) { fs_ = sampleRate, dirty_ = true, reset(); }
    void reset() {
        for (auto& ch : f_)
            for (auto& s : ch) s.reset();
    }
    void set(const Settings& s) {
        if (s.lowCutHz != s_.lowCutHz || s.lowDb != s_.lowDb || s.midHz != s_.midHz || s.midDb != s_.midDb || s.highDb != s_.highDb) dirty_ = true;
        s_ = s;
    }
    void update() {
        if (!dirty_) return;
        dirty_ = false;
        active_ = {s_.lowCutHz > 0.0f, std::abs(s_.lowDb) > 0.05f, std::abs(s_.midDb) > 0.05f, std::abs(s_.highDb) > 0.05f};
        for (auto& ch : f_) {
            ch[0].set(Svf::Type::Highpass, std::max(10.0f, s_.lowCutHz), 0.7f, 0.0f, fs_);
            ch[1].set(Svf::Type::LowShelf, 110.0f, 0.7f, s_.lowDb, fs_);
            ch[2].set(Svf::Type::Bell, s_.midHz, 0.9f, s_.midDb, fs_);
            ch[3].set(Svf::Type::HighShelf, 5500.0f, 0.7f, s_.highDb, fs_);
        }
    }
    void process(float& l, float& r) {
        for (size_t i = 0; i < 4; ++i)
            if (active_[i]) l = f_[0][i].process(l), r = f_[1][i].process(r);
    }

private:
    float fs_ = 48000.0f;
    Settings s_;
    bool dirty_ = true;
    std::array<bool, 4> active_{};
    std::array<std::array<Svf, 4>, 2> f_;
};

}  // namespace silt
