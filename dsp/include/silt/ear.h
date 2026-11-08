#pragma once

#include <array>

#include "common.h"

namespace silt {

// Who listens. Most of the "underwater sound" is here, not in the water: over the
// distances of a scene, sea water removes almost no treble (under 1 dB at 50 kHz over
// 50 m). A head in water hears through the skull (bone conduction): the ear canal fills,
// its ~3 kHz resonance is gone, sensitivity is a broad bowl centred on 0.4-1 kHz
// (Parvin, Nedwell 2007; Navy studies), and both cochleae get almost the same signal:
// the time between the ears shrinks 4.4x (c = 1500 m/s) and the head shadow vanishes,
// so the image collapses inside the head.
//   HYDROPHONE  a recorder's view: flat, full band, the real crackle of the sea.
//   DIVER       a bare head in water: the bowl, the lost canal resonance, narrow image.
//   HELMET      a hard-hat or full-face mask: an air pocket that rings, boxy and close.
//   CINEMA      the film convention: a steep, slightly resonant low-pass and a warm low end.
// AMOUNT goes from no colour to the full curve; CLARITY moves every corner up for
// intelligibility (a mermaid's lines, Aquaman's lesson). The level is matched: the ear
// changes the colour, not the loudness.
class Ear {
public:
    static constexpr int kModes = 4;
    static constexpr int kStages = 5;
    struct Stage {
        Svf::Type type;
        float hz, q, db;  // for lowpass / highpass: db unused
    };
    struct Mode {
        const char* name;
        std::array<Stage, kStages> stages;
        float width;     // stereo width it leaves (1 = untouched)
        float pocket;    // air-pocket comb (helmet)
    };
    static const std::array<Mode, kModes>& modes() {
        using T = Svf::Type;
        static const std::array<Mode, kModes> list = {{
            {"Hydrophone", {{{T::Highpass, 12.0f, 0.7f, 0}, {T::LowShelf, 90.0f, 0.7f, 2.0f}, {T::Bell, 2500.0f, 0.8f, 0.0f}, {T::HighShelf, 12000.0f, 0.7f, -1.5f}, {T::Lowpass, 20000.0f, 0.7f, 0}}}, 1.0f, 0.0f},
            {"Diver", {{{T::Highpass, 150.0f, 0.6f, 0}, {T::Bell, 650.0f, 0.55f, 4.0f}, {T::Bell, 3100.0f, 0.8f, -10.0f}, {T::HighShelf, 7500.0f, 0.7f, -8.0f}, {T::Lowpass, 5200.0f, 0.6f, 0}}}, 0.22f, 0.0f},
            {"Helmet", {{{T::Highpass, 190.0f, 0.7f, 0}, {T::Bell, 520.0f, 3.0f, 7.0f}, {T::Bell, 1650.0f, 5.0f, 6.0f}, {T::HighShelf, 3000.0f, 0.7f, -6.0f}, {T::Lowpass, 4200.0f, 0.8f, 0}}}, 0.5f, 1.0f},
            {"Cinema", {{{T::Highpass, 25.0f, 0.7f, 0}, {T::LowShelf, 110.0f, 0.7f, 3.0f}, {T::Bell, 320.0f, 0.9f, 2.0f}, {T::Lowpass, 680.0f, 1.1f, 0}, {T::Lowpass, 680.0f, 0.55f, 0}}}, 0.85f, 0.0f},
        }};
        return list;
    }

    struct Settings {
        int mode = 1;
        float amount = 0.85f, clarity = 0.2f, width = 1.0f;
    };

    void prepare(float sampleRate) {
        fs_ = sampleRate;
        for (auto& l : pocket_) l.prepare(int(0.004f * sampleRate) + 8);
        dirty_ = true;
        reset();
    }
    void reset() {
        for (auto& ch : svf_)
            for (auto& s : ch) s.reset();
        for (auto& l : pocket_) l.clear();
    }
    void set(const Settings& s) {
        if (s.mode != s_.mode || s.amount != s_.amount || s.clarity != s_.clarity || s.width != s_.width) dirty_ = true;
        s_ = s;
    }
    const Settings& settings() const { return s_; }

    // The stage as heard at this AMOUNT / CLARITY: gains scale, corners slide from
    // where they do nothing (20 kHz for a low-pass, 10 Hz for a high-pass).
    static Stage effective(const Stage& st, float amount, float clarity) {
        Stage e = st;
        const float up = std::pow(2.0f, 2.3f * clarity);  // up to ~5x higher
        switch (st.type) {
            case Svf::Type::Lowpass: e.hz = std::exp(std::log(20000.0f) + (std::log(std::min(st.hz * up, 20000.0f)) - std::log(20000.0f)) * amount); break;
            case Svf::Type::Highpass: e.hz = std::exp(std::log(10.0f) + (std::log(std::max(10.0f, st.hz / std::sqrt(up))) - std::log(10.0f)) * amount); break;
            default:
                e.db = st.db * amount * (st.db < 0.0f ? 1.0f - 0.6f * clarity : 1.0f);
                if (st.hz > 1000.0f) e.hz = std::min(st.hz * up, 18000.0f);
                break;
        }
        return e;
    }

    void update() {
        if (!dirty_) return;
        dirty_ = false;
        const Mode& m = modes()[size_t(std::clamp(s_.mode, 0, kModes - 1))];
        for (int i = 0; i < kStages; ++i) {
            const Stage e = effective(m.stages[size_t(i)], s_.amount, s_.clarity);
            for (auto& ch : svf_) ch[size_t(i)].set(e.type, e.hz, e.q, e.db, fs_);
        }
        width_ = (1.0f + (m.width - 1.0f) * s_.amount) * s_.width;
        pocketGain_ = m.pocket * s_.amount * 0.45f;
        pocketDelay_[0] = 0.00061f * fs_, pocketDelay_[1] = 0.00047f * fs_;
        // Level match: pink-noise power through the curve, weighted to the speech / music
        // band, brought back to 0 dB (at most +18).
        float num = 0.0f, den = 0.0f;
        for (int k = 0; k < 48; ++k) {
            const float hz = 31.5f * std::pow(2.0f, float(k) * 9.0f / 47.0f);
            const float w = hz < 100.0f ? hz / 100.0f : hz > 6000.0f ? 6000.0f / hz : 1.0f;
            float mag = 1.0f;
            for (int i = 0; i < kStages; ++i) mag *= svf_[0][size_t(i)].magnitude(hz, fs_);
            if (pocketGain_ > 0.0f) mag *= 1.0f + 0.5f * pocketGain_;
            num += w * mag * mag, den += w;
        }
        makeup_ = std::min(dbToGain(18.0f), 1.0f / std::sqrt(std::max(num / den, 1.0e-6f)));
    }

    void process(float& l, float& r) {
        for (int i = 0; i < kStages; ++i) {
            l = svf_[0][size_t(i)].process(l);
            r = svf_[1][size_t(i)].process(r);
        }
        if (pocketGain_ > 0.0f) {
            // Two short combs: the air in front of the face, a few cm across.
            const float pl = pocket_[0].readLinear(pocketDelay_[0]), pr = pocket_[1].readLinear(pocketDelay_[1]);
            pocket_[0].write(l + pocketGain_ * pl);
            pocket_[1].write(r + pocketGain_ * pr);
            l += pocketGain_ * pl, r += pocketGain_ * pr;
        }
        const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * width_;
        l = (mid + side) * makeup_;
        r = (mid - side) * makeup_;
    }

    float makeup() const { return makeup_; }
    // The curve the UI draws (dB at hz), level match included.
    float responseDb(float hz) const {
        float mag = makeup_;
        for (int i = 0; i < kStages; ++i) mag *= svf_[0][size_t(i)].magnitude(hz, fs_);
        return gainToDb(mag);
    }

private:
    float fs_ = 48000.0f;
    Settings s_;
    bool dirty_ = true;
    std::array<std::array<Svf, kStages>, 2> svf_;
    std::array<DelayLine, 2> pocket_;
    std::array<float, 2> pocketDelay_{};
    float width_ = 1.0f, pocketGain_ = 0.0f, makeup_ = 1.0f;
};

}  // namespace silt
