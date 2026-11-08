#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace fathom {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kWaterC = 1500.0f;  // m/s, sea water at ~15 °C (air: 343 m/s, 4.4 times slower)

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1.0e-9f)); }

// Per-pass gain of a loop of `delaySamples` so the level drops 60 dB in `rt60` seconds.
inline float rt60Gain(float delaySamples, float rt60, float sampleRate) {
    return std::pow(10.0f, -3.0f * delaySamples / (std::max(rt60, 1.0e-3f) * sampleRate));
}

// One-pole smoothing coefficient for a time constant, applied every `samples` samples.
inline float smoothingCoef(float timeSeconds, float sampleRate, int samples = 1) {
    return 1.0f - std::exp(-float(samples) / (std::max(timeSeconds, 1.0e-4f) * sampleRate));
}

inline void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));
#elif defined(__SSE__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

// xorshift32: cheap, deterministic, plenty for modulation and noise.
struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : state(seed ? seed : 1u) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float bipolar() { return uniform() * 2.0f - 1.0f; }                     // [-1, 1)
    float gauss() {                                                         // ~N(0, 1), Irwin-Hall
        return (uniform() + uniform() + uniform() + uniform() - 2.0f) * 1.7320508f;
    }
};

// Smooth random motion: a value that glides to a new random target every period
// (cosine ease), so it never jumps. Used for swell, sway and drift.
class Drift {
public:
    void seed(uint32_t s) { rng_ = Rng(s), from_ = rng_.bipolar(), to_ = rng_.bipolar(), t_ = rng_.uniform(); }
    // Advances by `dt` seconds at `hz` new targets per second; returns -1..1.
    float advance(float dt, float hz) {
        t_ += dt * hz;
        while (t_ >= 1.0f) t_ -= 1.0f, from_ = to_, to_ = rng_.bipolar();
        const float e = 0.5f - 0.5f * std::cos(kPi * t_);
        return from_ + (to_ - from_) * e;
    }

private:
    Rng rng_;
    float from_ = 0.0f, to_ = 0.0f, t_ = 0.0f;
};

// Power-of-two circular delay line. Read before write: delay 1 is the last written sample.
class DelayLine {
public:
    void prepare(int maxDelaySamples) {
        int size = 1;
        while (size < maxDelaySamples + 4) size <<= 1;
        buffer_.assign(size_t(size), 0.0f);
        mask_ = size - 1;
        writeIndex_ = 0;
    }
    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }
    int maxDelay() const { return mask_ - 3; }

    void write(float x) {
        buffer_[size_t(writeIndex_)] = x;
        writeIndex_ = (writeIndex_ + 1) & mask_;
    }
    float readInt(int delay) const { return buffer_[size_t((writeIndex_ - delay) & mask_)]; }
    float readLinear(float delay) const {
        delay = std::clamp(delay, 1.0f, float(maxDelay()));
        const int d = int(delay);
        const float t = delay - float(d);
        return readInt(d) + t * (readInt(d + 1) - readInt(d));
    }
    // Cubic Hermite: smooth under modulation.
    float read(float delay) const {
        delay = std::clamp(delay, 2.0f, float(maxDelay()));
        const int d = int(delay);
        const float t = delay - float(d);
        const float ym1 = readInt(d - 1), y0 = readInt(d), y1 = readInt(d + 1), y2 = readInt(d + 2);
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

private:
    std::vector<float> buffer_;
    int mask_ = 0;
    int writeIndex_ = 0;
};

// Topology-preserving one-pole (Zavalishin). Stable under fast modulation.
class OnePole {
public:
    void setCutoff(float hz, float sampleRate) {
        const float g = std::tan(kPi * std::clamp(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        a_ = g / (1.0f + g);
    }
    void setCoef(float a) { a_ = a; }
    float coef() const { return a_; }
    float lowpass(float x) {
        const float v = a_ * (x - s_);
        const float y = v + s_;
        s_ = y + v;
        return y;
    }
    float highpass(float x) { return x - lowpass(x); }
    void reset() { s_ = 0.0f; }

private:
    float a_ = 0.5f;
    float s_ = 0.0f;
};

// TPT state-variable filter (Simper / Cytomic) with the usual EQ shapes as output mixes.
// Zipper-free when its coefficients move (SUBMERGE sweeps every curve of the ear).
class Svf {
public:
    enum class Type { Lowpass, Highpass, Bandpass, Bell, LowShelf, HighShelf };
    void set(Type type, float hz, float q, float db, float sampleRate) {
        hz = std::clamp(hz, 5.0f, 0.48f * sampleRate);
        q = std::max(q, 0.05f);
        const float A = std::pow(10.0f, db / 40.0f);
        float g = std::tan(kPi * hz / sampleRate);
        float k = 1.0f / q;
        switch (type) {
            case Type::Lowpass: m0_ = 0.0f, m1_ = 0.0f, m2_ = 1.0f; break;
            case Type::Highpass: m0_ = 1.0f, m1_ = -k, m2_ = -1.0f; break;
            case Type::Bandpass: m0_ = 0.0f, m1_ = 1.0f, m2_ = 0.0f; break;
            case Type::Bell: k = 1.0f / (q * A), m0_ = 1.0f, m1_ = k * (A * A - 1.0f), m2_ = 0.0f; break;
            case Type::LowShelf: g /= std::sqrt(A), m0_ = 1.0f, m1_ = k * (A - 1.0f), m2_ = A * A - 1.0f; break;
            case Type::HighShelf: g *= std::sqrt(A), m0_ = A * A, m1_ = k * (1.0f - A) * A, m2_ = 1.0f - A * A; break;
        }
        g_ = g, k_ = k;
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    float process(float v0) {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return m0_ * v0 + m1_ * v1 + m2_ * v2;
    }
    void reset() { ic1_ = ic2_ = 0.0f; }
    // Magnitude response at `hz` (for the level match and the UI's ear curve).
    float magnitude(float hz, float sampleRate) const {
        // H(z) of the trapezoidal SVF, evaluated on the unit circle via the analog prototype.
        const float w = std::tan(kPi * std::min(hz, 0.49f * sampleRate) / sampleRate) / g_;  // prewarped s = j w
        // H(s) = (m0 s^2 + (m0 k + m1) s + (m0 + m2)) / (s^2 + k s + 1) with s normalised.
        const float nr = (m0_ + m2_) - m0_ * w * w, ni = (m0_ * k_ + m1_) * w;
        const float dr = 1.0f - w * w, di = k_ * w;
        return std::sqrt((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1.0e-12f));
    }

private:
    float g_ = 0.1f, k_ = 1.4f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float m0_ = 1.0f, m1_ = 0.0f, m2_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

}  // namespace fathom
