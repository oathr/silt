#pragma once

#include <silt/ear.h>
#include <silt/echo.h>
#include <silt/multipath.h>
#include <silt/tone.h>
#include <silt/water_tail.h>

#include "Params.h"

namespace silt {

// The whole chain, without JUCE (the render tool runs it too):
//   input -> PATHS (image method, real absorption, waveguide cut) -> MUFFLE
//         -> + ECHO (a far wall) + WATER (reverberation) -> EAR -> EQ
//         -> SUBMERGE crossfade with the dry, + a parallel DRY, WET level -> safety limiter
// Latency: none (made for the master bus). The dry is never delayed; the wet carries
// only SWAY's few ms. A module that is off computes nothing.
class Engine {
public:
    static constexpr int kBlock = MultiPath::kBlock;
    // +2 dB on the wet: at WET 0 dB the presets land within ~2 dB of the dry (measured on pink noise).
    static constexpr float kWetTrim = 1.2589f;

    Engine() {
        static const char* const ids[] = {"submerge", "muffle", "dry", "wet", "distance", "src_depth", "lis_depth", "water_depth", "sway",
                                          "path_on", "bottom", "waves", "reflections", "echo_on", "echo_wall", "echo_dist", "echo_fb",
                                          "echo_level", "water_on", "space", "size", "decay", "tone", "water_level", "ear", "ear_amount",
                                          "clarity", "width", "eq_lowcut", "eq_low", "eq_mid_freq", "eq_mid", "eq_high"};
        static_assert(sizeof(ids) / sizeof(ids[0]) == size_t(Ix::Count));
        for (size_t i = 0; i < size_t(Ix::Count); ++i) ix_[i] = params::indexOf(ids[i]);
    }

    void prepare(float sampleRate) {
        enableFlushToZero();
        fs_ = sampleRate;
        paths_.prepare(sampleRate);
        muffle_.prepare(sampleRate);
        echo_.prepare(sampleRate);
        tail_.prepare(sampleRate);
        ear_.prepare(sampleRate);
        eq_.prepare(sampleRate);
        smooth_ = smoothingCoef(0.03f, sampleRate);
        release_ = smoothingCoef(0.15f, sampleRate);
        meter_ = smoothingCoef(0.08f, sampleRate);
        for (auto& b : bands_) b.reset();
        bands_[0].setCutoff(250.0f, sampleRate), bands_[1].setCutoff(2500.0f, sampleRate);
        first_ = true;
        counter_ = 0;
        needsReset_.store(false);
    }

    void reset() {
        paths_.reset(), muffle_.reset(), echo_.reset(), tail_.reset(), ear_.reset(), eq_.reset();
        limit_ = 1.0f;
        counter_ = 0;
        first_ = true;
        needsReset_.store(false, std::memory_order_relaxed);
    }

    // values: one per params::all() entry (0..1 knobs, 0/1 switches, choice index).
    void process(const float* v, float* left, float* right, int numSamples) {
        using namespace params;
        const auto at = [&](Ix i) { return v[ix_[size_t(i)]]; };
        const auto choice = [&](Ix i) { return int(std::lround(at(i))); };

        submergeTarget_ = at(Ix::Submerge);
        dryTarget_ = levelGain(at(Ix::Dry));
        wetTarget_ = levelGain(at(Ix::Wet)) * kWetTrim;
        muffleT_ = at(Ix::Muffle);

        MultiPath::Settings ps;
        ps.range = distanceM(at(Ix::Distance));
        ps.srcDepth = depthM(at(Ix::SrcDepth));
        ps.lisDepth = depthM(at(Ix::LisDepth));
        ps.waterDepth = waterDepthM(at(Ix::WaterDepth));
        ps.bottom = choice(Ix::Bottom);
        ps.waves = wavesM(at(Ix::Waves));
        ps.sway = swayM(at(Ix::Sway));
        ps.reflections = reflectionsX(at(Ix::Reflections));
        ps.murk = 1.0f;
        ps.width = std::min(1.0f, widthX(at(Ix::Width)));
        ps.bounces = at(Ix::PathOn) > 0.5f;
        paths_.set(ps);

        echoOn_ = at(Ix::EchoOn) > 0.5f && at(Ix::EchoLevel) > 0.0f;
        echoTarget_ = echoOn_ ? levelGain(at(Ix::EchoLevel)) : 0.0f;
        Echo::Settings es;
        es.distanceM = echoDistanceM(at(Ix::EchoDist));
        es.feedback = 0.92f * at(Ix::EchoFb);
        es.wall = choice(Ix::EchoWall);
        es.width = std::min(1.0f, widthX(at(Ix::Width)));
        es.muffleHz = muffleCornerHz(muffleT_);
        es.waves = std::min(1.0f, ps.waves / 0.6f);
        echo_.set(es);

        tailOn_ = at(Ix::WaterOn) > 0.5f && at(Ix::WaterLevel) > 0.0f;
        tailTarget_ = tailOn_ ? levelGain(at(Ix::WaterLevel)) : 0.0f;
        WaterTail::Settings ts;
        ts.space = choice(Ix::Space);
        ts.sizeM = spaceSizeM(at(Ix::Size));
        ts.decayS = decayS(at(Ix::Decay));
        // MUFFLE darkens the tail too (its treble decays faster).
        ts.tone = at(Ix::Tone) * (1.0f - 0.8f * muffleT_);
        ts.motion = std::min(1.0f, ps.waves / 0.6f);
        ts.predelayMs = std::round((ps.bounces ? paths_.firstBounceMs() : 0.0f) + 3.0f);
        tail_.set(ts);

        earSettings_.mode = choice(Ix::Ear);
        earSettings_.amount = at(Ix::EarAmount);
        earSettings_.clarity = at(Ix::Clarity);
        earSettings_.width = widthX(at(Ix::Width));

        Eq::Settings q;
        q.lowCutHz = lowCutHz(at(Ix::EqLowCut));
        q.lowDb = (2.0f * at(Ix::EqLow) - 1.0f) * 12.0f;
        q.midHz = midHz(at(Ix::EqMidFreq));
        q.midDb = (2.0f * at(Ix::EqMid) - 1.0f) * 12.0f;
        q.highDb = (2.0f * at(Ix::EqHigh) - 1.0f) * 12.0f;
        eq_.set(q);

        if (first_) {
            submerge_ = submergeTarget_, dry_ = dryTarget_, wet_ = wetTarget_;
            echoGain_ = echoTarget_, tailGain_ = tailTarget_, muffleCur_ = muffleT_;
        }

        float peakIn = 0.0f, peakOut = 0.0f;
        for (int n = 0; n < numSamples; ++n) {
            if (counter_ == 0) updateBlock();
            counter_ = (counter_ + 1) % kBlock;

            const float dryL = left[n], dryR = right[n];
            peakIn = std::max(peakIn, std::max(std::abs(dryL), std::abs(dryR)));

            float wl, wr;
            paths_.process(dryL, dryR, wl, wr);
            muffle_.process(wl, wr);

            float el = 0.0f, er = 0.0f;
            if (echoOn_ || echoGain_ > 1.0e-5f) {
                echo_.process(wl, wr, el, er);
                el *= echoGain_, er *= echoGain_;
            }
            if (tailOn_ || tailGain_ > 1.0e-5f) {
                float tl, tr;
                tail_.process(wl + 0.5f * el, wr + 0.5f * er, tl, tr);
                wl += tailGain_ * tl, wr += tailGain_ * tr;
            }
            wl += el, wr += er;
            ear_.process(wl, wr);
            eq_.process(wl, wr);

            // SUBMERGE: equal-power crossfade from the surface (dry) to the water (wet, at WET).
            // DRY is a parallel dry on top, heard whatever SUBMERGE says (off by default).
            const float a = 0.5f * kPi * submerge_;
            const float dryGain = std::cos(a) + dry_;
            float l = dryGain * dryL + std::sin(a) * wet_ * wl;
            float r = dryGain * dryR + std::sin(a) * wet_ * wr;

            // Safety: instant gain reduction above -0.3 dBFS, 150 ms recovery.
            const float peak = std::max(std::abs(l), std::abs(r));
            if (peak * limit_ > 0.966f) limit_ = 0.966f / peak;
            else limit_ += (1.0f - limit_) * release_;
            l *= limit_, r *= limit_;
            if (!std::isfinite(l) || !std::isfinite(r)) {
                l = r = 0.0f;
                needsReset_.store(true, std::memory_order_relaxed);
            }
            left[n] = l, right[n] = r;
            peakOut = std::max(peakOut, std::min(peak, 4.0f));

            const float m = 0.5f * (wl + wr);
            const float low = bands_[0].lowpass(m), lowMid = bands_[1].lowpass(m);
            band_[0] += (low * low - band_[0]) * meter_;
            band_[1] += ((lowMid - low) * (lowMid - low) - band_[1]) * meter_;
            band_[2] += ((m - lowMid) * (m - lowMid) - band_[2]) * meter_;
        }
        if (needsReset_.load(std::memory_order_relaxed)) reset();
        inLevel_.store(std::max(peakIn, inLevel_.load(std::memory_order_relaxed) * 0.8f), std::memory_order_relaxed);
        outLevel_.store(std::max(peakOut, outLevel_.load(std::memory_order_relaxed) * 0.8f), std::memory_order_relaxed);
        for (int b = 0; b < 3; ++b) bandLevel_[size_t(b)].store(std::sqrt(band_[size_t(b)]), std::memory_order_relaxed);
        first_ = false;
    }

    // For the UI (any thread).
    const MultiPath& paths() const { return paths_; }
    float inLevel() const { return inLevel_.load(std::memory_order_relaxed); }
    float outLevel() const { return outLevel_.load(std::memory_order_relaxed); }
    float band(int b) const { return bandLevel_[size_t(b)].load(std::memory_order_relaxed); }
    float echoMs() const { return echo_.delayMs(); }
    bool needsReset() const { return needsReset_.load(std::memory_order_relaxed); }

private:
    enum class Ix { Submerge, Muffle, Dry, Wet, Distance, SrcDepth, LisDepth, WaterDepth, Sway, PathOn, Bottom, Waves,
                    Reflections, EchoOn, EchoWall, EchoDist, EchoFb, EchoLevel, WaterOn, Space, Size, Decay, Tone,
                    WaterLevel, Ear, EarAmount, Clarity, Width, EqLowCut, EqLow, EqMidFreq, EqMid, EqHigh, Count };

    void updateBlock() {
        const float k = first_ ? 1.0f : 1.0f - std::pow(1.0f - smooth_, float(kBlock));
        const auto glide = [&](float& v, float t) {
            v += (t - v) * k;
            if (std::abs(t - v) < 1.0e-4f) v = t;
        };
        glide(submerge_, submergeTarget_);
        glide(dry_, dryTarget_);
        glide(wet_, wetTarget_);
        glide(echoGain_, echoTarget_);
        glide(tailGain_, tailTarget_);
        glide(muffleCur_, muffleT_);
        paths_.update();
        muffle_.set(muffleCornerHz(muffleCur_), muffleCur_);
        if (echoOn_ || echoGain_ > 1.0e-5f) echo_.update(kBlock);
        if (tailOn_ || tailGain_ > 1.0e-5f) tail_.update();
        Ear::Settings e = earSettings_;
        e.amount *= submerge_;  // a plunge: the ear closes as the head goes under
        ear_.set(e);
        ear_.update();
        eq_.update();
    }

    float fs_ = 48000.0f;
    std::array<int, size_t(Ix::Count)> ix_{};
    MultiPath paths_;
    Muffle muffle_;
    Echo echo_;
    WaterTail tail_;
    Ear ear_;
    Ear::Settings earSettings_;
    Eq eq_;
    std::array<OnePole, 2> bands_;
    std::array<float, 3> band_{};
    std::array<std::atomic<float>, 3> bandLevel_{};
    std::atomic<float> inLevel_{0.0f}, outLevel_{0.0f};
    std::atomic<bool> needsReset_{false};

    float submerge_ = 1.0f, submergeTarget_ = 1.0f, dry_ = 1.0f, dryTarget_ = 1.0f, wet_ = 1.0f, wetTarget_ = 1.0f;
    float echoGain_ = 0.0f, echoTarget_ = 0.0f, tailGain_ = 0.0f, tailTarget_ = 0.0f, muffleT_ = 0.0f, muffleCur_ = 0.0f;
    float smooth_ = 0.001f, release_ = 0.0001f, meter_ = 0.001f, limit_ = 1.0f;
    bool echoOn_ = false, tailOn_ = true, first_ = true;
    int counter_ = 0;
};

}  // namespace silt
