#pragma once

#include <array>

#include "common.h"

namespace fathom {

// The late, diffuse field of a body of water: a 16-line feedback delay network with a
// Hadamard matrix (every line feeds every other: dense from the first milliseconds).
// Its delays come from the size in metres at the speed of sound in water (1500 m/s), so
// a 10 m pool rings 4.4 times denser than a 10 m room in air: underwater spaces sound
// close and fluttery, which is why an air reverb never sounds right.
// Open water barely reverberates: OPEN SEA and ABYSS are sparse scattering tails
// (plankton, bubbles, temperature layers), not halls.
class WaterTail {
public:
    struct Space {
        const char* name;
        float spread;      // how unequal the line lengths are (low = regular flutter, tiles)
        float trebleRatio; // treble decay / bass decay
        float diffusion;   // input allpass gain
        float modMs;       // line modulation depth (moving water)
        float lowCut;      // Hz
        float resonance;   // structure ringing (hull plates, glass), 0..1
        std::array<float, 4> modes;  // its frequencies, Hz
    };
    static constexpr int kSpaces = 7;
    static const std::array<Space, kSpaces>& spaces() {
        static const std::array<Space, kSpaces> list = {{
            {"Open sea", 0.65f, 0.30f, 0.78f, 0.60f, 60.0f, 0.0f, {0, 0, 0, 0}},
            {"Bay", 0.55f, 0.45f, 0.72f, 0.40f, 40.0f, 0.0f, {0, 0, 0, 0}},
            {"Pool", 0.30f, 0.80f, 0.55f, 0.12f, 50.0f, 0.0f, {0, 0, 0, 0}},
            {"Cave", 0.75f, 0.42f, 0.80f, 0.22f, 30.0f, 0.15f, {95.0f, 160.0f, 240.0f, 410.0f}},
            {"Wreck", 0.45f, 0.60f, 0.65f, 0.18f, 45.0f, 1.0f, {118.0f, 287.0f, 523.0f, 911.0f}},
            {"Tank", 0.28f, 0.85f, 0.50f, 0.05f, 120.0f, 0.55f, {780.0f, 1630.0f, 2710.0f, 4230.0f}},
            {"Abyss", 0.80f, 0.20f, 0.85f, 0.90f, 25.0f, 0.0f, {0, 0, 0, 0}},
        }};
        return list;
    }

    struct Settings {
        int space = 1;
        float sizeM = 20.0f, decayS = 2.0f, tone = 0.4f, predelayMs = 10.0f, motion = 0.2f;
    };

    void prepare(float sampleRate) {
        fs_ = sampleRate;
        for (auto& l : lines_) l.prepare(int(0.5f * sampleRate) + 64);
        for (auto& a : diffusers_) a.prepare(int(0.04f * sampleRate) + 8);
        predelay_.prepare(int(0.25f * sampleRate) + 8);
        for (int i = 0; i < kLines; ++i) phase_[size_t(i)] = float(i) / kLines;
        dirty_ = true;
        reset();
    }
    void reset() {
        for (auto& l : lines_) l.clear();
        for (auto& a : diffusers_) a.clear();
        predelay_.clear();
        for (auto& d : damp_) d.reset();
        for (auto& f : lowCut_) f.reset();
        for (auto& m : modes_) m.reset();
    }
    void set(const Settings& s) {
        if (s.space != s_.space || s.sizeM != s_.sizeM || s.decayS != s_.decayS || s.tone != s_.tone || s.predelayMs != s_.predelayMs || s.motion != s_.motion) dirty_ = true;
        s_ = s;
    }

    void update() {
        if (!dirty_) return;
        dirty_ = false;
        const Space& sp = spaces()[size_t(std::clamp(s_.space, 0, kSpaces - 1))];
        // Mutually prime-ish ratios spread over an octave and a half.
        static constexpr std::array<float, kLines> ratios = {0.56f, 0.61f, 0.66f, 0.71f, 0.77f, 0.83f, 0.89f, 0.95f,
                                                             1.02f, 1.09f, 1.17f, 1.25f, 1.33f, 1.42f, 1.52f, 1.63f};
        const float mean = std::max(0.0012f * fs_, 0.6f * s_.sizeM / kWaterC * fs_);
        const float trebleCorner = 600.0f * std::pow(14.0f, s_.tone);
        const float trebleDecay = s_.decayS * sp.trebleRatio * (0.55f + 0.9f * s_.tone);
        for (int i = 0; i < kLines; ++i) {
            const float d = std::min(mean * std::pow(ratios[size_t(i)], 1.0f + 2.0f * sp.spread), 0.45f * fs_);
            base_[size_t(i)] = d;
            lowGain_[size_t(i)] = rt60Gain(d, s_.decayS, fs_);
            highGain_[size_t(i)] = rt60Gain(d, trebleDecay, fs_);
            damp_[size_t(i)].setCutoff(trebleCorner, fs_);
            rate_[size_t(i)] = (0.05f + 0.023f * float(i)) * (0.5f + s_.motion);
        }
        modDepth_ = sp.modMs * 0.001f * fs_ * (0.4f + 1.2f * s_.motion);
        static constexpr std::array<float, 4> diffMs = {1.3f, 2.3f, 3.7f, 5.3f};
        const float diffScale = std::clamp(s_.sizeM / 20.0f, 0.3f, 4.0f);
        for (size_t i = 0; i < diffusers_.size(); ++i) diffDelay_[i] = std::min(diffMs[i] * diffScale * 0.001f * fs_, 0.038f * fs_);
        diffusion_ = sp.diffusion;
        for (auto& f : lowCut_) f.setCutoff(sp.lowCut, fs_);
        resonance_ = sp.resonance;
        for (size_t m = 0; m < modes_.size(); ++m)
            if (sp.modes[m] > 0.0f) modes_[m].set(Svf::Type::Bandpass, sp.modes[m], 18.0f + 6.0f * float(m), 0.0f, fs_);
        predelaySamples_ = std::clamp(s_.predelayMs * 0.001f * fs_, 1.0f, float(predelay_.maxDelay()));
        // Loudness roughly constant across decays (fathom_render --analyse).
        outGain_ = 0.30f / std::sqrt(std::max(0.25f, s_.decayS));
    }

    void process(float inL, float inR, float& outL, float& outR) {
        predelay_.write(0.5f * (inL + inR));
        float x = predelay_.read(predelaySamples_);
        const float side = 0.5f * (inL - inR);
        for (size_t i = 0; i < diffusers_.size(); ++i) {
            auto& line = diffusers_[i];
            const float d = line.read(diffDelay_[i]);
            const float v = x + diffusion_ * d;
            line.write(v);
            x = d - diffusion_ * v;
        }
        float y[kLines];
        for (int i = 0; i < kLines; ++i) {
            float& ph = phase_[size_t(i)];
            ph += rate_[size_t(i)] / fs_;
            if (ph >= 1.0f) ph -= 1.0f;
            const float mod = modDepth_ * (0.5f + 0.5f * std::sin(kTwoPi * ph));
            float v = lines_[size_t(i)].read(base_[size_t(i)] + mod + 2.0f);
            const float low = damp_[size_t(i)].lowpass(v);
            y[i] = lowGain_[size_t(i)] * low + highGain_[size_t(i)] * (v - low);
        }
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            const float s = (i * 7 + 3) % 5 < 2 ? -1.0f : 1.0f;  // decorrelated taps
            (i & 1 ? r : l) += s * y[i];
        }
        // Fast Walsh-Hadamard transform, scaled to stay lossless (1/4 for 16).
        for (int len = 1; len < kLines; len <<= 1)
            for (int i = 0; i < kLines; i += len << 1)
                for (int j = i; j < i + len; ++j) {
                    const float a = y[j], b = y[j + len];
                    y[j] = a + b, y[j + len] = a - b;
                }
        for (int i = 0; i < kLines; ++i) {
            const float in = x * ((i & 1) ? 0.3f : -0.3f) + side * ((i & 2) ? 0.22f : -0.22f);
            lines_[size_t(i)].write(0.25f * y[i] + in);
        }
        if (resonance_ > 0.0f) {
            float ring = 0.0f;
            for (auto& m : modes_) ring += m.process(x + 0.1f * (l + r));
            l += resonance_ * 0.5f * ring, r += resonance_ * 0.5f * ring;
        }
        outL = lowCut_[0].highpass(l) * outGain_;
        outR = lowCut_[1].highpass(r) * outGain_;
    }

private:
    static constexpr int kLines = 16;
    float fs_ = 48000.0f;
    Settings s_;
    bool dirty_ = true;
    std::array<DelayLine, kLines> lines_;
    std::array<DelayLine, 4> diffusers_;
    DelayLine predelay_;
    std::array<float, kLines> base_{}, lowGain_{}, highGain_{}, rate_{}, phase_{};
    std::array<OnePole, kLines> damp_;
    std::array<float, 4> diffDelay_{};
    std::array<OnePole, 2> lowCut_;
    std::array<Svf, 4> modes_;
    float modDepth_ = 0.0f, diffusion_ = 0.6f, resonance_ = 0.0f, predelaySamples_ = 1.0f, outGain_ = 0.3f;
};

}  // namespace fathom
