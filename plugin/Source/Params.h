#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Every parameter of SILT, in one table shared by the processor (APVTS layout), the
// editor (one web relay each), the engine (value by index) and the render tool.
// Continuous parameters are stored 0..1, the knob's position; the functions below turn
// them into real units, and `text` shows the same label as the UI and the DAW.
// SILT only transforms the sound it gets: nothing is generated.
namespace silt::params {

enum class Kind { Float, Bool, Choice };

struct Spec {
    std::string id;
    std::string name;
    Kind kind;
    float defaultValue;                     // 0..1, false/true, or choice index
    std::vector<std::string> choices = {};  // Choice only
    std::function<std::string(float)> text = {};
};

inline std::string fmt(const char* f, double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}
inline std::string pct(float t) { return fmt("%.0f %%", 100.0 * t); }
inline std::string metres(float m) { return m < 10.0f ? fmt("%.1f m", m) : m < 1000.0f ? fmt("%.0f m", m) : fmt("%.2f km", m / 1000.0f); }
inline std::string bipolarDb(float t, float range) { return fmt("%+.1f dB", (2.0 * t - 1.0) * range); }

// Units shared by the engine and the labels. Depths reach the bottom of the Mariana
// Trench (10 994 m).
inline float distanceM(float t) { return 0.3f * std::pow(33333.3f, t); }      // 0.3 m .. 10 km
inline float depthM(float t) { return 0.1f * std::pow(110000.0f, t); }        // 0.1 m .. 11 km
inline float waterDepthM(float t) { return std::pow(11000.0f, t); }           // 1 m .. 11 km
inline float wavesM(float t) { return 1.5f * t * t; }                         // rms height 0 .. 1.5 m
inline float swayM(float t) { return 4.0f * t * t; }                          // 0 .. 4 m
inline float reflectionsX(float t) { return 2.0f * t; }                       // x0 .. x2 (1 = physical)
inline float echoDistanceM(float t) { return 5.0f * std::pow(600.0f, t); }    // 5 m .. 3 km (6.7 ms .. 4 s)
inline float spaceSizeM(float t) { return 2.0f * std::pow(100.0f, t); }       // 2 .. 200 m
inline float decayS(float t) { return 0.2f * std::pow(100.0f, t); }           // 0.2 .. 20 s
inline float widthX(float t) { return 2.0f * t; }                             // 0 .. 200 %
inline float lowCutHz(float t) { return t <= 0.0f ? 0.0f : 20.0f * std::pow(25.0f, t); }  // off, 20 .. 500 Hz
inline float midHz(float t) { return 100.0f * std::pow(80.0f, t); }           // 100 Hz .. 8 kHz
// Levels: 0 = off, then -48 .. +12 dB.
inline float levelDb(float t) { return -48.0f + 60.0f * t; }
inline float levelGain(float t) { return t <= 0.0f ? 0.0f : std::pow(10.0f, levelDb(t) / 20.0f); }
// Inverse maps, for the presets (written in real units).
inline float tDistance(float m) { return std::log(m / 0.3f) / std::log(33333.3f); }
inline float tDepth(float m) { return std::log(m / 0.1f) / std::log(110000.0f); }
inline float tWaterDepth(float m) { return std::log(m) / std::log(11000.0f); }
inline float tWaves(float m) { return std::sqrt(m / 1.5f); }
inline float tSway(float m) { return std::sqrt(m / 4.0f); }
inline float tEcho(float m) { return std::log(m / 5.0f) / std::log(600.0f); }
inline float tSize(float m) { return std::log(m / 2.0f) / std::log(100.0f); }
inline float tDecay(float s) { return std::log(s / 0.2f) / std::log(100.0f); }
inline float tLevel(float db) { return (db + 48.0f) / 60.0f; }
inline float tMid(float hz) { return std::log(hz / 100.0f) / std::log(80.0f); }
inline float tBipolar(float db, float range) { return 0.5f + 0.5f * db / range; }

inline std::string levelText(float t) { return t <= 0.0f ? std::string("off") : fmt("%+.1f dB", levelDb(t)); }
inline std::string hzText(float hz) { return hz < 1000.0f ? fmt("%.0f Hz", hz) : fmt("%.1f kHz", hz / 1000.0f); }

inline const std::vector<std::string> kBottoms = {"Mud", "Sand", "Gravel", "Rock", "Reef"};
inline const std::vector<std::string> kWalls = {"Cliff", "Reef", "Hull", "Ice"};
inline const std::vector<std::string> kSpaces = {"Open sea", "Bay", "Pool", "Cave", "Wreck", "Tank", "Abyss"};
inline const std::vector<std::string> kEars = {"Hydrophone", "Diver", "Helmet", "Cinema"};

inline const std::vector<Spec>& all() {
    static const std::vector<Spec> specs = {
        // Global: the plunge, the muffling, the two levels
        {"submerge", "Submerge", Kind::Float, 1.0f, {}, pct},
        {"muffle", "Muffle", Kind::Float, 0.0f, {}, [](float t) {
             if (t <= 0.0f) return std::string("real water");
             const float hz = 20000.0f * std::pow(90.0f / 20000.0f, std::pow(t, 0.75f));
             return hzText(hz);
         }},
        // DRY: a parallel dry added on top of SUBMERGE's crossfade (off = only the crossfade).
        {"dry", "Dry Level", Kind::Float, 0.0f, {}, levelText},
        {"wet", "Wet Level", Kind::Float, tLevel(0.0f), {}, levelText},
        // SOURCE: where the sound is, from where it is heard
        {"distance", "Distance", Kind::Float, tDistance(4.0f), {}, [](float t) { return metres(distanceM(t)); }},
        {"src_depth", "Source Depth", Kind::Float, tDepth(3.0f), {}, [](float t) { return metres(depthM(t)); }},
        {"lis_depth", "Listener Depth", Kind::Float, tDepth(4.0f), {}, [](float t) { return metres(depthM(t)); }},
        {"water_depth", "Water Depth", Kind::Float, tWaterDepth(12.0f), {}, [](float t) { return metres(waterDepthM(t)); }},
        {"sway", "Sway", Kind::Float, tSway(0.3f), {}, [](float t) { return t <= 0.0f ? std::string("still") : fmt("%.2f m", swayM(t)); }},
        // PATHS: surface and floor bounces
        {"path_on", "Paths On", Kind::Bool, 1.0f},
        {"bottom", "Bottom", Kind::Choice, 1.0f, kBottoms},
        {"waves", "Waves", Kind::Float, tWaves(0.15f), {}, [](float t) { return t <= 0.0f ? std::string("flat") : fmt("%.2f m", wavesM(t)); }},
        {"reflections", "Reflections", Kind::Float, 0.5f, {}, [](float t) { return fmt("x%.2f", reflectionsX(t)); }},
        // ECHO: a wall far away
        {"echo_on", "Echo On", Kind::Bool, 0.0f},
        {"echo_wall", "Echo Wall", Kind::Choice, 0.0f, kWalls},
        {"echo_dist", "Echo Distance", Kind::Float, tEcho(60.0f), {}, [](float t) {
             const float m = echoDistanceM(t);
             return metres(m) + fmt(", %.0f ms", 2000.0 * m / 1500.0);
         }},
        {"echo_fb", "Echo Feedback", Kind::Float, 0.3f, {}, pct},
        {"echo_level", "Echo Level", Kind::Float, tLevel(-8.0f), {}, levelText},
        // WATER: the reverberation
        {"water_on", "Water On", Kind::Bool, 1.0f},
        {"space", "Space", Kind::Choice, 1.0f, kSpaces},
        {"size", "Size", Kind::Float, tSize(20.0f), {}, [](float t) { return metres(spaceSizeM(t)); }},
        {"decay", "Decay", Kind::Float, tDecay(2.0f), {}, [](float t) { return fmt(decayS(t) < 10.0f ? "%.1f s" : "%.0f s", decayS(t)); }},
        {"tone", "Tone", Kind::Float, 0.4f, {}, pct},
        {"water_level", "Reverb Level", Kind::Float, tLevel(-6.0f), {}, levelText},
        // LISTENER
        {"ear", "Ear", Kind::Choice, 1.0f, kEars},
        {"ear_amount", "Ear Amount", Kind::Float, 0.85f, {}, pct},
        {"clarity", "Clarity", Kind::Float, 0.2f, {}, pct},
        {"width", "Width", Kind::Float, 0.5f, {}, [](float t) { return pct(widthX(t)); }},
        // EQ (on the wet)
        {"eq_lowcut", "EQ Low Cut", Kind::Float, 0.0f, {}, [](float t) { return t <= 0.0f ? std::string("off") : hzText(lowCutHz(t)); }},
        {"eq_low", "EQ Low", Kind::Float, 0.5f, {}, [](float t) { return bipolarDb(t, 12.0f); }},
        {"eq_mid_freq", "EQ Mid Freq", Kind::Float, tMid(800.0f), {}, [](float t) { return hzText(midHz(t)); }},
        {"eq_mid", "EQ Mid", Kind::Float, 0.5f, {}, [](float t) { return bipolarDb(t, 12.0f); }},
        {"eq_high", "EQ High", Kind::Float, 0.5f, {}, [](float t) { return bipolarDb(t, 12.0f); }},
    };
    return specs;
}

inline int indexOf(const std::string& id) {
    const auto& s = all();
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i].id == id) return int(i);
    return -1;
}

}  // namespace silt::params
