#pragma once

#include "common.h"

namespace silt {

// Sea water absorption, Francois & Garrison (JASA 72, 1982): boric acid, magnesium
// sulphate and pure water relaxations. dB per km, f in Hz. Defaults: 15 °C, 35 ‰, pH 8.
inline float absorptionDbPerKm(float hz, float depthM = 10.0f, float tempC = 15.0f, float salinity = 35.0f, float ph = 8.0f) {
    const double f = hz * 1.0e-3, T = tempC, S = salinity, D = depthM;
    const double c = 1412.0 + 3.21 * T + 1.19 * S + 0.0167 * D;
    const double A1 = 8.86 / c * std::pow(10.0, 0.78 * ph - 5.0);
    const double f1 = 2.8 * std::sqrt(S / 35.0) * std::pow(10.0, 4.0 - 1245.0 / (T + 273.0));
    const double A2 = 21.44 * S / c * (1.0 + 0.025 * T);
    const double P2 = 1.0 - 1.37e-4 * D + 6.2e-9 * D * D;
    const double f2 = 8.17 * std::pow(10.0, 8.0 - 1990.0 / (T + 273.0)) / (1.0 + 0.0018 * (S - 35.0));
    const double A3 = 4.937e-4 - 2.59e-5 * T + 9.11e-7 * T * T - 1.50e-8 * T * T * T;
    const double P3 = 1.0 - 3.83e-5 * D + 4.9e-10 * D * D;
    const double f2sq = f * f;
    return float(A1 * f1 * f2sq / (f1 * f1 + f2sq) + A2 * P2 * f2 * f2sq / (f2 * f2 + f2sq) + A3 * P3 * f2sq);
}

// The frequency where a path of `rangeM` metres (absorption scaled by `murk`) has lost
// 3 dB: the corner of the lowpass that stands for it. 0 = nothing audible to remove.
inline float absorptionCornerHz(float rangeM, float murk, float depthM) {
    const auto loss = [&](float hz) { return absorptionDbPerKm(hz, depthM) * rangeM * 1.0e-3f * murk; };
    if (loss(22000.0f) < 3.0f) return 0.0f;
    float lo = std::log(50.0f), hi = std::log(22000.0f);
    if (loss(50.0f) >= 3.0f) return 50.0f;
    for (int i = 0; i < 30; ++i) {
        const float mid = 0.5f * (lo + hi);
        (loss(std::exp(mid)) < 3.0f ? lo : hi) = mid;
    }
    return std::exp(0.5f * (lo + hi));
}

}  // namespace silt
