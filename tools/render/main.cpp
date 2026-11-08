// fathom_render: runs the FATHOM engine offline.
//   fathom_render --analyse                     measurements (physics checks, level, CPU, safety)
//   fathom_render <in.wav> <out dir> [preset]   renders the file through every preset (or one)
//   fathom_render --demo <out dir>              same, with built-in test sounds
#include <chrono>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>

#include "../../plugin/Source/Engine.h"
#include "../../plugin/Source/Presets.h"
#include "wav.h"

using namespace fathom;

namespace {

std::vector<float> defaults() { return presets::valuesOf(presets::Preset{"", "", "", {}}); }
void setValue(std::vector<float>& v, const char* id, float x) { v[size_t(params::indexOf(id))] = x; }

void run(Engine& e, const std::vector<float>& v, Stereo& audio, int block = 256) {
    for (size_t at = 0; at < audio.size(); at += size_t(block)) {
        const int n = int(std::min<size_t>(size_t(block), audio.size() - at));
        e.process(v.data(), audio.left.data() + at, audio.right.data() + at, n);
    }
}

double rmsDb(const std::vector<float>& x, size_t from = 0) {
    double s = 0.0;
    for (size_t i = from; i < x.size(); ++i) s += double(x[i]) * x[i];
    return 10.0 * std::log10(s / double(std::max<size_t>(1, x.size() - from)) + 1.0e-20);
}

// Magnitude of the response h at hz (direct DFT), dB.
double responseDb(const std::vector<float>& h, double hz, double fs) {
    std::complex<double> acc = 0.0;
    for (size_t i = 0; i < h.size(); ++i) acc += double(h[i]) * std::polar(1.0, -2.0 * 3.14159265358979 * hz * double(i) / fs);
    return 20.0 * std::log10(std::abs(acc) + 1.0e-12);
}

// The wet alone, flat: paths only, hydrophone at amount 0, no tail / bubbles / sea.
std::vector<float> pathsOnly() {
    auto v = defaults();
    setValue(v, "ear", 0), setValue(v, "ear_amount", 0.0f), setValue(v, "water_on", 0.0f);
    setValue(v, "sway", 0.0f), setValue(v, "waves", 0.0f), setValue(v, "width", 0.0f);  // width 0: no ITD between taps
    return v;
}

Stereo pinkNoise(float fs, float seconds, float db) {
    Stereo a;
    a.sampleRate = fs;
    a.resize(size_t(fs * seconds));
    Rng rng(42);
    float b0 = 0, b1 = 0, b2 = 0;
    const float g = std::pow(10.0f, db / 20.0f) * 3.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        const float w = rng.bipolar();
        b0 = 0.99765f * b0 + w * 0.0990460f, b1 = 0.96300f * b1 + w * 0.2965164f, b2 = 0.57000f * b2 + w * 1.0526913f;
        a.left[i] = a.right[i] = (b0 + b1 + b2 + w * 0.1848f) * 0.25f * g;
    }
    return a;
}

// Test sounds for --demo: a sung vowel glide, knocks, a breathy noise sweep.
Stereo demoSource(float fs) {
    Stereo a;
    a.sampleRate = fs;
    a.resize(size_t(fs * 9.0f));
    Rng rng(7);
    double phase = 0.0;
    Svf f1, f2, f3;
    for (size_t i = 0; i < a.size(); ++i) {
        const float t = float(i) / fs;
        float y = 0.0f;
        if (t < 3.5f) {  // voice: a glottal-ish pulse through three formants
            const float hz = 220.0f * std::pow(2.0f, 0.25f * std::sin(1.3f * t)) * (1.0f + 0.006f * std::sin(kTwoPi * 5.5f * t));
            phase += hz / fs;
            const float saw = float(phase - std::floor(phase)) * 2.0f - 1.0f;
            const float vowel = 0.5f + 0.5f * std::sin(0.9f * t);
            f1.set(Svf::Type::Bandpass, 500.0f + 300.0f * vowel, 6.0f, 0, fs), f2.set(Svf::Type::Bandpass, 1100.0f + 900.0f * vowel, 8.0f, 0, fs);
            f3.set(Svf::Type::Bandpass, 2600.0f, 10.0f, 0, fs);
            const float env = std::min(1.0f, t * 8.0f) * std::min(1.0f, (3.5f - t) * 4.0f);
            y = 0.5f * env * (f1.process(saw) + 0.6f * f2.process(saw) + 0.3f * f3.process(saw));
        } else if (t < 6.0f) {  // knocks: a wooden-ish impact every 0.6 s
            const float u = std::fmod(t - 3.5f, 0.6f);
            y = 0.8f * std::exp(-u * 60.0f) * (std::sin(kTwoPi * 180.0f * u) + 0.5f * std::sin(kTwoPi * 610.0f * u) + 0.3f * rng.bipolar() * std::exp(-u * 300.0f));
        } else {  // a breath of noise, swelling
            const float u = (t - 6.0f) / 3.0f;
            y = 0.3f * std::sin(3.14159f * u) * rng.bipolar();
        }
        a.left[i] = a.right[i] = y;
    }
    return a;
}

int analyse() {
    const float fs = 48000.0f;
    std::printf("FATHOM measurements, %.0f Hz\n\n", fs);

    std::printf("1. Sea water absorption (Francois-Garrison, 15 C, 35 ppt, 10 m)\n");
    for (float hz : {1000.0f, 10000.0f, 50000.0f}) std::printf("   %6.0f Hz : %6.3f dB/km\n", hz, absorptionDbPerKm(hz));
    std::printf("   lowpass corner (-3 dB): 50 m -> %.0f Hz, 3 km -> %.0f Hz, 10 km -> %.0f Hz (0 = none)\n\n",
                absorptionCornerHz(50, 1, 10), absorptionCornerHz(3000, 1, 10), absorptionCornerHz(10000, 1, 10));

    std::printf("2. Arrival times (image method) vs geometry: r = 10 m, source 2 m, listener 3 m, water 8 m, sand\n");
    {
        Engine e;
        e.prepare(fs);
        auto v = pathsOnly();
        setValue(v, "distance", params::tDistance(10)), setValue(v, "src_depth", params::tDepth(2)), setValue(v, "lis_depth", params::tDepth(3));
        setValue(v, "water_depth", params::tWaterDepth(8)), setValue(v, "submerge", 1.0f);
        Stereo a;
        a.sampleRate = fs;
        a.resize(size_t(fs * 0.2f));
        a.left[0] = a.right[0] = 1.0f;
        run(e, v, a);
        const double r0 = std::hypot(10.0, 1.0);
        const double expect[] = {0.0, (std::hypot(10.0, 5.0) - r0) / 1500.0, (std::hypot(10.0, 11.0) - r0) / 1500.0};
        const char* names[] = {"direct", "surface (inverted)", "bottom"};
        // The direct path sits at the wet's offset (4 samples): find it, then the others relative.
        size_t direct = 0;
        for (size_t i = 0; i < 64; ++i)
            if (std::abs(a.left[i]) > std::abs(a.left[direct])) direct = i;
        for (int k = 0; k < 3; ++k) {
            const size_t at = direct + size_t(std::lround(expect[k] * fs));
            size_t best = at;
            for (size_t i = at > 6 ? at - 6 : 0; i < at + 6; ++i)
                if (std::abs(a.left[i]) > std::abs(a.left[best])) best = i;
            std::printf("   %-20s expected %6.3f ms, measured %6.3f ms, sign %+.0f\n", names[k], expect[k] * 1000.0,
                        double(best - direct) / fs * 1000.0, a.left[best] >= 0 ? 1.0 : -1.0);
        }
        std::printf("\n");
    }

    std::printf("3. Lloyd's mirror: source and listener 0.5 m below the surface, 20 m apart, 60 m of water\n");
    {
        Engine e;
        e.prepare(fs);
        auto v = pathsOnly();
        setValue(v, "distance", params::tDistance(20)), setValue(v, "src_depth", params::tDepth(0.5f)), setValue(v, "lis_depth", params::tDepth(0.5f));
        setValue(v, "water_depth", params::tWaterDepth(60)), setValue(v, "bottom", 0);
        Stereo a;
        a.sampleRate = fs;
        a.resize(size_t(fs * 0.5f));
        a.left[0] = a.right[0] = 1.0f;
        run(e, v, a);
        std::printf("   measured:       ");
        for (double hz : {50.0, 100.0, 200.0, 400.0, 800.0}) std::printf("  %4.0f Hz %+5.1f dB", hz, responseDb(a.left, hz, fs));
        // Theory (two paths, 1/R): |1 - (R0/R1) e^{-jk(R1-R0)}| / |1 + R0/R1| at high f.
        const double r0 = 20.0, r1 = std::hypot(20.0, 1.0);
        std::printf("\n   two-path theory:");
        for (double hz : {50.0, 100.0, 200.0, 400.0, 800.0}) {
            const double k = 2.0 * 3.14159265 * hz / 1500.0;
            const auto h = 1.0 - (r0 / r1) * std::polar(1.0, -k * (r1 - r0));
            std::printf("  %4.0f Hz %+5.1f dB", hz, 20.0 * std::log10(std::abs(h) / std::sqrt(1.0 + (r0 / r1) * (r0 / r1))));
        }
        std::printf("\n   (the measured curve also holds the far bottom bounces of 60 m of mud)\n\n");
    }

    std::printf("4. ECHO timing (2 d / c) and MUFFLE corners\n");
    {
        Engine e;
        e.prepare(fs);
        auto v = pathsOnly();
        setValue(v, "path_on", 0.0f), setValue(v, "echo_on", 1.0f), setValue(v, "echo_dist", params::tEcho(75)), setValue(v, "echo_fb", 0.0f);
        setValue(v, "echo_level", params::tLevel(0)), setValue(v, "echo_wall", 3);
        Stereo a;
        a.sampleRate = fs;
        a.resize(size_t(fs * 0.3f));
        a.left[0] = a.right[0] = 1.0f;
        run(e, v, a);
        size_t best = 200;
        for (size_t i = 200; i < a.size(); ++i)
            if (std::abs(a.left[i]) + std::abs(a.right[i]) > std::abs(a.left[best]) + std::abs(a.right[best])) best = i;
        std::printf("   wall at 75 m: expected 100.0 ms, measured %.1f ms\n", double(best) / fs * 1000.0 - 4.0 / fs * 1000.0);
        std::printf("   MUFFLE corner:");
        for (float t : {0.25f, 0.5f, 0.75f, 1.0f}) std::printf("  %3.0f %% -> %5.0f Hz", t * 100.0f, muffleCornerHz(t));
        std::printf("\n\n");
    }
    std::printf("4b. DRY alone (SUBMERGE 100 %%, WET off, DRY 0 dB): output re input\n");
    {
        Engine e;
        e.prepare(fs);
        auto v = defaults();
        setValue(v, "wet", 0.0f), setValue(v, "dry", params::tLevel(0)), setValue(v, "submerge", 1.0f);
        Stereo a = pinkNoise(fs, 2.0f, -18.0f);
        const double in = rmsDb(a.left, size_t(fs / 2));
        run(e, v, a);
        std::printf("   %+.2f dB (expected 0)\n\n", rmsDb(a.left, size_t(fs / 2)) - in);
    }
    std::printf("5. Level (pink noise at -18 dBFS RMS in): output RMS re input, per preset\n");
    std::printf("6. CPU: seconds of audio per second of compute (realtime x), 48 kHz, 256-sample blocks\n");
    const auto noise = pinkNoise(fs, 8.0f, -18.0f);
    const double inDb = rmsDb(noise.left, size_t(fs));
    for (const auto& p : presets::all()) {
        Engine e;
        e.prepare(fs);
        auto v = presets::valuesOf(p);
        setValue(v, "submerge", 1.0f);
        Stereo a = noise;
        const auto t0 = std::chrono::steady_clock::now();
        run(e, v, a);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %-28s %+5.1f dB   x%5.0f realtime\n", p.name, rmsDb(a.left, size_t(fs)) - inDb, 8.0 / secs);
    }
    std::printf("\n");

    std::printf("7. Safety: 60 s of random settings every 0.25 s on loud noise, buffers 1..2048\n");
    {
        Engine e;
        e.prepare(96000.0f);
        Rng rng(99);
        const auto& specs = params::all();
        std::vector<float> v = defaults();
        Stereo a = pinkNoise(96000.0f, 60.0f, -3.0f);
        float peak = 0.0f;
        bool finite = true;
        for (size_t at = 0; at < a.size();) {
            if ((at / 24000) != ((at + 1) / 24000) || at == 0)
                for (size_t i = 0; i < specs.size(); ++i)
                    v[i] = specs[i].kind == params::Kind::Choice ? float(int(rng.uniform() * specs[i].choices.size())) : specs[i].kind == params::Kind::Bool ? float(rng.uniform() < 0.8f) : rng.uniform();
            const int n = int(std::min<size_t>(1 + rng.next() % 2048, a.size() - at));
            e.process(v.data(), a.left.data() + at, a.right.data() + at, n);
            for (size_t i = at; i < at + size_t(n); ++i) {
                finite &= std::isfinite(a.left[i]) && std::isfinite(a.right[i]);
                peak = std::max(peak, std::max(std::abs(a.left[i]), std::abs(a.right[i])));
            }
            at += size_t(n);
        }
        std::printf("   finite: %s, peak %.3f (%.2f dBFS)\n", finite ? "yes" : "NO", peak, 20.0 * std::log10(peak));
    }
    return 0;
}

int render(Stereo source, const std::string& outDir, const std::string& only) {
    std::filesystem::create_directories(outDir);
    writeWav(outDir + "/00 dry.wav", source);
    int index = 1;
    for (const auto& p : presets::all()) {
        const std::string name = p.name;
        if (!only.empty() && name != only) { ++index; continue; }
        Engine e;
        e.prepare(source.sampleRate);
        auto v = presets::valuesOf(p);
        setValue(v, "submerge", 1.0f);  // the Plongeon preset starts dry: render it under
        Stereo a = source;
        // A tail: 3 s of silence after the source.
        a.left.resize(a.size() + size_t(3 * a.sampleRate)), a.right.resize(a.left.size());
        run(e, v, a);
        char file[512];
        std::snprintf(file, sizeof file, "%s/%02d %s.wav", outDir.c_str(), index++, p.name);
        writeWav(file, a);
        std::printf("%s\n", file);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "--analyse";
    if (first == "--analyse") return analyse();
    if (first == "--demo") return render(demoSource(48000.0f), argc > 2 ? argv[2] : "renders", argc > 3 ? argv[3] : "");
    if (argc < 3) {
        std::printf("usage: fathom_render --analyse | --demo <dir> [preset] | <in.wav> <dir> [preset]\n");
        return 1;
    }
    Stereo source;
    std::string error;
    if (!readWav(argv[1], source, error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    return render(source, argv[2], argc > 3 ? argv[3] : "");
}
