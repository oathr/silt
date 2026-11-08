#pragma once

#include <array>
#include <atomic>

#include "absorption.h"
#include "common.h"

namespace fathom {

// Sea floors: density (water = 1), sound speed, extra loss per bounce (attenuation in the
// sediment) and rms roughness in metres (scatters the treble of each bounce).
// Speeds and densities: Hamilton / Jensen et al. tables (fine sand 1725 m/s, 2.05; silty
// mud 1475 m/s, 1.55). Normal-incidence reflection: sand ~0.4, mud ~0.2, rock ~0.7.
struct Seabed {
    const char* name;
    float density, speed, loss, roughness;
};
inline const std::array<Seabed, 5> kSeabeds = {{
    {"Mud", 1.55f, 1475.0f, 0.80f, 0.02f},
    {"Sand", 2.05f, 1725.0f, 0.88f, 0.03f},
    {"Gravel", 2.10f, 1850.0f, 0.85f, 0.08f},
    {"Rock", 2.60f, 3500.0f, 0.95f, 0.12f},
    {"Reef", 2.20f, 2600.0f, 0.78f, 0.35f},
}};

// Rayleigh reflection coefficient of a fluid bottom at a grazing angle (sin given).
// Below the critical angle (faster bottoms) the reflection is total; its phase shift is
// left out (a frequency-independent rotation the ear barely tells from the sign).
inline float bottomReflection(const Seabed& b, float sinGrazing) {
    const float n = kWaterC / b.speed;
    const float under = n * n - (1.0f - sinGrazing * sinGrazing);
    if (under <= 0.0f) return b.loss;
    const float z = std::sqrt(under), ms = b.density * sinGrazing;
    return b.loss * (ms - z) / (ms + z);
}

// Lowest propagating mode of the water column (Pekeris waveguide): below it, sound from
// further than a few depths away fades out. f = c / (4 H sqrt(1 - c²/cb²)); a bottom
// slower than water guides nothing: it leaks every mode, taken as a rigid floor x1.5.
inline float waveguideCutoffHz(float waterDepth, const Seabed& b) {
    const float ratio = kWaterC / b.speed;
    const float k = ratio < 0.999f ? std::sqrt(1.0f - ratio * ratio) : 0.67f;
    return kWaterC / (4.0f * std::max(waterDepth, 0.5f) * k);
}

// Image method in a layer of water between a pressure-release surface (reflection -1,
// treble scattered by waves) and a fluid floor. Every arrival is a tap on a delay line:
// its delay is the extra path length over c = 1500 m/s, its gain spherical spreading x
// the reflections, its colour two one-poles standing for the coherent loss of rough
// boundaries, exp(-2 (k h sin θ)²) per bounce (Eckart / Kirchhoff).
// The direct path has no delay beyond SWAY's travel: the dry and the wet stay in sync.
// A source near the surface meets its own inverted image: the low end cancels (Lloyd's
// mirror) and comes out of the taps by itself, as does the comb of a shallow bay.
class MultiPath {
public:
    static constexpr int kOrders = 12;
    static constexpr int kTaps = 4 * kOrders;  // 4 families x orders; tap 0 is the direct path
    static constexpr int kBlock = 32;           // samples between two geometry updates

    struct Settings {
        float range = 4.0f, srcDepth = 3.0f, lisDepth = 4.0f, waterDepth = 12.0f;
        int bottom = 1;
        float waves = 0.15f;      // rms surface height, m
        float sway = 0.3f;        // how far the source wanders, m
        float reflections = 1.0f; // level of every bounce (1 = physical)
        float murk = 1.0f;        // absorption x (1 = sea water)
        float width = 1.0f;
        bool bounces = true;      // false: the direct path only
    };

    // What the UI draws: each arrival, relative to the direct one.
    struct Arrival {
        std::atomic<float> ms{0.0f}, gain{0.0f};
        std::atomic<int> surface{0}, bottom{0};
    };

    void prepare(float sampleRate) {
        fs_ = sampleRate;
        const int maxSamples = int(2.2f * sampleRate);
        for (auto& l : lines_) l.prepare(maxSamples);
        maxDelay_ = float(lines_[0].maxDelay() - 8);
        Rng rng(0xF47A0001u);
        for (int i = 0; i < kTaps; ++i) {
            Tap& t = taps_[size_t(i)];
            const int family = i % 4, order = i / 4;
            t.ns = family == 0 ? order : family == 1 ? order + 1 : family == 2 ? order : order + 1;
            t.nb = family == 0 ? order : family == 1 ? order : order + 1;
            t.family = family, t.order = order;
            t.pan = i == 0 ? 0.0f : rng.bipolar();
            t.swell.seed(0x1000u + uint32_t(i) * 7919u);
            t.chop.seed(0x2000u + uint32_t(i) * 104729u);
            t.glint.seed(0x3000u + uint32_t(i) * 1299709u);
        }
        swayX_.seed(0xABCDu), swayZ_.seed(0x1234u);
        first_ = true;
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.clear();
        for (auto& t : taps_) {
            for (auto& ch : t.lp)
                for (auto& p : ch) p.reset();
            t.gain = t.gainStep = 0.0f;
        }
        for (auto& f : post_) f.reset();
        first_ = true;
    }

    void set(const Settings& s) { target_ = s; }

    // Once every kBlock samples: geometry, taps and filters for the next block.
    void update() {
        const float dt = float(kBlock) / fs_;
        const float k = first_ ? 1.0f : smoothingCoef(0.08f, fs_, kBlock);
        auto glide = [&](float& v, float t) { v += (t - v) * k; };
        // Distances glide in log (a knob turned from 1 m to 1 km does not sweep linearly).
        auto glideLog = [&](float& v, float t) { v = std::exp(std::log(v) + (std::log(t) - std::log(v)) * k); };
        if (first_) cur_ = target_;
        glideLog(cur_.range, target_.range);
        glideLog(cur_.waterDepth, target_.waterDepth);
        glideLog(cur_.srcDepth, target_.srcDepth);
        glideLog(cur_.lisDepth, target_.lisDepth);
        glide(cur_.waves, target_.waves);
        glide(cur_.sway, target_.sway);
        glide(cur_.reflections, target_.reflections);
        glide(cur_.width, target_.width);
        cur_.murk = target_.murk, cur_.bottom = target_.bottom, cur_.bounces = target_.bounces;

        const Seabed& bed = kSeabeds[size_t(std::clamp(cur_.bottom, 0, int(kSeabeds.size()) - 1))];
        const float H = cur_.waterDepth;
        const float margin = std::min(0.05f, 0.25f * H);
        const float zsNominal = std::clamp(cur_.srcDepth, margin, H - margin);
        const float zr = std::clamp(cur_.lisDepth, margin, H - margin);

        // SWAY: the source drifts around its place (a slow random walk, ~0.07 Hz).
        const float sx = swayX_.advance(dt, 0.07f), sz = swayZ_.advance(dt, 0.05f);
        const float r = std::max(0.2f, cur_.range + cur_.sway * sx);
        const float zs = std::clamp(zsNominal + 0.5f * cur_.sway * sz, margin, H - margin);
        const float r0Nominal = std::hypot(cur_.range, zsNominal - zr);
        const float r0 = std::hypot(r, zs - zr);
        // Room for SWAY to come closer than the nominal distance: the only delay of the wet.
        const float offset = 4.0f + 1.6f * target_.sway / kWaterC * fs_;

        const float waves = cur_.waves;
        float energy = 0.0f;
        for (int i = 0; i < kTaps; ++i) {
            Tap& t = taps_[size_t(i)];
            const float o = float(t.order);
            float v;
            switch (t.family) {
                case 0: v = 2.0f * o * H + zs - zr; break;        // n bottoms, n surfaces (direct at n = 0)
                case 1: v = 2.0f * o * H + zs + zr; break;        // n bottoms, n + 1 surfaces
                case 2: v = 2.0f * (o + 1.0f) * H - zs - zr; break;  // n + 1 bottoms, n surfaces
                default: v = 2.0f * (o + 1.0f) * H - zs + zr; break; // n + 1 of each
            }
            v = std::abs(v);
            const float length = std::hypot(r, v);
            const float sinG = v / std::max(length, 1.0e-3f);
            float gain = r0 / length;
            float pathMod = 0.0f;
            float cutoff = 0.0f;  // 0 = full band
            if (i > 0) {
                if (!cur_.bounces) gain = 0.0f;
                gain *= cur_.reflections;
                if (t.ns & 1) gain = -gain;
                const float rb = bottomReflection(bed, sinG);
                for (int b = 0; b < t.nb; ++b) gain *= rb;
                if (t.ns > 0 && waves > 0.0f) {
                    // The surface moves: each bounce point rises and falls with the swell
                    // (~0.12 Hz) and the chop (~1.1 Hz), lengthening the path by 2 η sin θ;
                    // focusing by the moving surface makes the level flicker.
                    const float eta = waves * (0.85f * t.swell.advance(dt, 0.12f) + 0.35f * t.chop.advance(dt, 1.1f));
                    pathMod = 2.0f * eta * sinG * std::sqrt(float(t.ns));
                    gain *= 1.0f + std::min(0.5f, 0.6f * waves) * t.glint.advance(dt, 0.6f);
                }
                // Treble scattered away by rough boundaries: -3 dB where 2 n (k h sin θ)² = ln √2.
                float inv = 0.0f;
                const auto add = [&](float h, int n) {
                    if (h <= 0.0f || n == 0) return;
                    const float f3 = 99.3f / (h * std::max(sinG, 1.0e-3f) * std::sqrt(float(n)));
                    inv += 1.0f / (f3 * f3);
                };
                add(waves, t.ns);
                add(bed.roughness, t.nb);
                if (inv > 0.0f) cutoff = 1.0f / std::sqrt(inv);
            }
            float delay = offset + (length - r0Nominal + pathMod) / kWaterC * fs_;
            if (delay > maxDelay_ - 64.0f) gain = 0.0f;
            delay = std::min(delay, maxDelay_ - 64.0f);
            energy += gain * gain;

            // Per sample glides to the new values across the block.
            if (first_) t.delay = delay, t.gain = gain;
            t.delayStep = (delay - t.delay) / float(kBlock);
            t.gainStep = (gain - t.gain) / float(kBlock);
            t.filtered = cutoff > 0.0f && cutoff < 16000.0f;
            if (t.filtered)
                for (auto& ch : t.lp)
                    for (auto& p : ch) p.setCutoff(std::max(cutoff * 1.55f, 40.0f), fs_);
            t.itd = t.pan * cur_.width * 0.00012f * fs_;  // ~0.12 ms between the ears, at most

            if (i < kArrivalsShown) {
                arrivals_[size_t(i)].ms.store((length - r0) / kWaterC * 1000.0f, std::memory_order_relaxed);
                arrivals_[size_t(i)].gain.store(gain, std::memory_order_relaxed);
                arrivals_[size_t(i)].surface.store(t.ns, std::memory_order_relaxed);
                arrivals_[size_t(i)].bottom.store(t.nb, std::memory_order_relaxed);
            }
        }
        // Constant loudness whatever the geometry: a shallow bay sums many strong paths.
        const float norm = 1.0f / std::sqrt(std::max(energy, 1.0e-6f));
        if (first_) norm_ = norm;
        normStep_ = (norm - norm_) / float(kBlock);

        // After the sum: absorption (Francois-Garrison x MURK) and the waveguide's low cut,
        // which grows from nothing at one depth away to the full mode cutoff at ten.
        const float absorption = absorptionCornerHz(r0, cur_.murk, 0.5f * H);
        absorbing_ = absorption > 0.0f;
        const float guided = std::clamp((r / H - 1.0f) / 9.0f, 0.0f, 1.0f);
        const float modal = guided * waveguideCutoffHz(H, bed);
        modalHz_ = modal;
        highpassing_ = cur_.bounces && modal > 15.0f;
        for (int ch = 0; ch < 2; ++ch) {
            if (absorbing_) post_[size_t(ch)].set(Svf::Type::Lowpass, absorption, 0.5f, 0.0f, fs_);
            if (highpassing_) post_[size_t(2 + ch)].set(Svf::Type::Highpass, modal, 0.6f, 0.0f, fs_);
        }
        absorptionHz_ = absorption;
        first_ = false;
    }

    void process(float inL, float inR, float& outL, float& outR) {
        lines_[0].write(inL);
        lines_[1].write(inR);
        float l = 0.0f, r = 0.0f;
        const int count = cur_.bounces ? kTaps : 1;
        for (int i = 0; i < count; ++i) {
            Tap& t = taps_[size_t(i)];
            t.delay += t.delayStep;
            t.gain += t.gainStep;
            if (std::abs(t.gain) < 1.0e-5f && std::abs(t.gainStep) < 1.0e-7f) continue;
            float a = lines_[0].readLinear(t.delay + t.itd);
            float b = lines_[1].readLinear(t.delay - t.itd);
            if (t.filtered) {
                a = t.lp[0][1].lowpass(t.lp[0][0].lowpass(a));
                b = t.lp[1][1].lowpass(t.lp[1][0].lowpass(b));
            }
            const float bal = 0.4f * t.pan * cur_.width;
            l += t.gain * (1.0f + bal) * a;
            r += t.gain * (1.0f - bal) * b;
        }
        norm_ += normStep_;
        l *= norm_, r *= norm_;
        if (absorbing_) l = post_[0].process(l), r = post_[1].process(r);
        if (highpassing_) l = post_[2].process(l), r = post_[3].process(r);
        outL = l, outR = r;
    }

    static constexpr int kArrivalsShown = kTaps;
    const Arrival& arrival(int i) const { return arrivals_[size_t(i)]; }
    float absorptionHz() const { return absorptionHz_; }
    float modalHz() const { return modalHz_; }
    // Tap delay of the direct path, samples (the wet's only latency, from SWAY).
    float directDelay() const { return taps_[0].delay; }
    // Time from the direct sound to the first strong bounce (for the late tail's pre-delay).
    float firstBounceMs() const { return std::max(0.0f, arrivals_[1].ms.load(std::memory_order_relaxed)); }

private:
    struct Tap {
        int ns = 0, nb = 0, family = 0, order = 0;
        float pan = 0.0f, itd = 0.0f;
        float delay = 4.0f, delayStep = 0.0f, gain = 0.0f, gainStep = 0.0f;
        bool filtered = false;
        OnePole lp[2][2];
        Drift swell, chop, glint;
    };

    float fs_ = 48000.0f, maxDelay_ = 1000.0f;
    std::array<DelayLine, 2> lines_;
    std::array<Tap, kTaps> taps_;
    std::array<Arrival, kArrivalsShown> arrivals_;
    std::array<Svf, 4> post_;
    Settings target_, cur_;
    Drift swayX_, swayZ_;
    float norm_ = 1.0f, normStep_ = 0.0f, absorptionHz_ = 0.0f, modalHz_ = 0.0f;
    bool absorbing_ = false, highpassing_ = false, first_ = true;
};

}  // namespace fathom
