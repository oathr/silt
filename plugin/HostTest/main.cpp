// silt_hosttest <SILT.vst3> [--no-timing]
// Loads the VST3 the way a DAW does and stresses it: three sample rates, buffers from 64
// to 2048 (and odd sizes), random automation, every program, a state round trip, mono
// input. Fails on NaN / Inf, on a peak over 0 dBFS, on a state that does not come back,
// and (unless --no-timing) on a block slower than its real-time budget.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

namespace {

int failures = 0;
void check(bool ok, const juce::String& what) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL %s\n", what.toRawUTF8());
    }
}

void fillNoise(juce::AudioBuffer<float>& b, juce::Random& rng, float level) {
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(c, i, (rng.nextFloat() * 2.0f - 1.0f) * level);
}

// The plugin's own parameters: not the host-side Bypass / Program parameters the VST3
// wrapper adds (they are not part of the plugin's state).
std::vector<juce::AudioProcessorParameter*> ownParameters(juce::AudioPluginInstance& p) {
    std::vector<juce::AudioProcessorParameter*> list;
    for (auto* param : p.getParameters()) {
        const auto name = param->getName(64);
        if (param == p.getBypassParameter() || name == "Bypass" || name == "Program") continue;
        list.push_back(param);
    }
    return list;
}

struct Result {
    float peak = 0.0f;
    bool finite = true;
    double worstLoad = 0.0;
};

Result runFor(juce::AudioPluginInstance& p, double seconds, int block, double rate, juce::Random& rng, bool automate, bool varyBlocks) {
    Result r;
    juce::AudioBuffer<float> buffer(2, 2048);
    juce::MidiBuffer midi;
    const auto params = ownParameters(p);
    const int total = int(seconds * rate);
    for (int done = 0; done < total;) {
        const int n = varyBlocks ? 1 + rng.nextInt(2048) : block;
        buffer.setSize(2, n, false, false, true);
        fillNoise(buffer, rng, 0.5f);
        if (automate && rng.nextFloat() < 0.2f) {
            auto* param = params[size_t(rng.nextInt(int(params.size())))];
            param->setValue(rng.nextFloat());
        }
        const auto t0 = juce::Time::getHighResolutionTicks();
        p.processBlock(buffer, midi);
        const double used = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
        r.worstLoad = std::max(r.worstLoad, used / (n / rate));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const float s = buffer.getSample(c, i);
                r.finite &= std::isfinite(s);
                r.peak = std::max(r.peak, std::abs(s));
            }
        done += n;
    }
    return r;
}

// --compat <old.vst3> <new.vst3>: an older build (e.g. FATHOM, the plugin's first name) and
// this one must share their VST3 ID, and every state the old one saves (each program, and
// random settings) must load into the new one with the same values.
int compat(const char* oldPath, const char* newPath) {
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> a, b;
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    vst3.findAllTypesForFile(a, cwd.getChildFile(oldPath).getFullPathName());
    vst3.findAllTypesForFile(b, cwd.getChildFile(newPath).getFullPathName());
    if (a.isEmpty() || b.isEmpty()) return std::printf("plugin not found\n"), 2;
    std::printf("old: %s, id %08x\nnew: %s, id %08x\n", a[0]->name.toRawUTF8(), unsigned(a[0]->uniqueId), b[0]->name.toRawUTF8(), unsigned(b[0]->uniqueId));
    check(a[0]->uniqueId == b[0]->uniqueId && a[0]->deprecatedUid == b[0]->deprecatedUid, "the VST3 IDs differ: hosts will not find old sessions");
    juce::String error;
    auto oldPlugin = formats.createPluginInstance(*a[0], 48000.0, 512, error);
    auto newPlugin = formats.createPluginInstance(*b[0], 48000.0, 512, error);
    if (oldPlugin == nullptr || newPlugin == nullptr) return std::printf("cannot load: %s\n", error.toRawUTF8()), 2;
    const auto oldParams = ownParameters(*oldPlugin), newParams = ownParameters(*newPlugin);
    check(oldParams.size() == newParams.size(), "parameter count differs");
    juce::Random rng(77);
    int states = 0, mismatches = 0;
    for (int i = 0; i < oldPlugin->getNumPrograms() + 20; ++i) {
        if (i < oldPlugin->getNumPrograms()) oldPlugin->setCurrentProgram(i);
        else
            for (auto* p : oldParams) {
                const int steps = p->getNumSteps();
                float v = rng.nextFloat();
                if (steps > 1 && steps < 100) v = float(juce::roundToInt(v * float(steps - 1))) / float(steps - 1);
                p->setValue(v);
            }
        juce::MemoryBlock state;
        oldPlugin->getStateInformation(state);
        newPlugin->setStateInformation(state.getData(), int(state.getSize()));
        for (size_t k = 0; k < std::min(oldParams.size(), newParams.size()); ++k)
            if (std::abs(oldParams[k]->getValue() - newParams[k]->getValue()) > 1.0e-4f) {
                if (mismatches++ < 5) std::printf("  state %d: %s %.4f -> %.4f\n", i, oldParams[k]->getName(32).toRawUTF8(), oldParams[k]->getValue(), newParams[k]->getValue());
            }
        ++states;
    }
    check(mismatches == 0, juce::String(mismatches) + " values changed");
    std::printf("%d old states loaded into the new build\n", states);
    std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc >= 4 && juce::String(argv[1]) == "--compat") return compat(argv[2], argv[3]);
    if (argc < 2) {
        std::printf("usage: silt_hosttest <SILT.vst3> [--no-timing] | --compat <old.vst3> <new.vst3>\n");
        return 2;
    }
    const bool timing = !(argc > 2 && juce::String(argv[2]) == "--no-timing");
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]).getFullPathName());
    if (found.isEmpty()) {
        std::printf("no plugin found in %s\n", argv[1]);
        return 2;
    }
    juce::Random rng(1234);
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        juce::String error;
        auto plugin = formats.createPluginInstance(*found[0], rate, 512, error);
        if (plugin == nullptr) {
            std::printf("cannot load: %s\n", error.toRawUTF8());
            return 2;
        }
        std::printf("%s at %.0f Hz: %d parameters, %d programs\n", plugin->getName().toRawUTF8(), rate, plugin->getParameters().size(), plugin->getNumPrograms());
        plugin->setPlayConfigDetails(2, 2, rate, 2048);
        plugin->prepareToPlay(rate, 2048);

        for (const int block : {64, 128, 256, 512, 1024, 2048}) {
            const auto r = runFor(*plugin, 2.0, block, rate, rng, true, false);
            check(r.finite, "NaN/Inf, block " + juce::String(block));
            check(r.peak <= 1.0f, "peak " + juce::String(r.peak) + ", block " + juce::String(block));
            if (timing) check(r.worstLoad < 1.0, "block " + juce::String(block) + " took " + juce::String(r.worstLoad * 100.0, 0) + " % of its budget");
            std::printf("  block %4d: peak %.3f, worst block %5.1f %% of budget\n", block, r.peak, r.worstLoad * 100.0);
        }
        const auto odd = runFor(*plugin, 5.0, 0, rate, rng, true, true);
        check(odd.finite && odd.peak <= 1.0f, "odd block sizes");

        for (int i = 0; i < plugin->getNumPrograms(); ++i) {
            plugin->setCurrentProgram(i);
            const auto r = runFor(*plugin, 1.5, 512, rate, rng, false, false);
            check(r.finite && r.peak <= 1.0f, "program " + juce::String(i) + " " + plugin->getProgramName(i));
            if (rate == 48000.0) std::printf("  program %2d %-40s peak %.3f, worst %5.1f %%\n", i, plugin->getProgramName(i).toRawUTF8(), r.peak, r.worstLoad * 100.0);
        }

        // State round trip: a random state saved, scrambled, restored.
        const auto own = ownParameters(*plugin);
        // Switches and menus get exact steps (a host never sends them between two).
        const auto randomise = [&] {
            for (auto* param : own) {
                const int steps = param->getNumSteps();
                float v = rng.nextFloat();
                if (steps > 1 && steps < 100) v = float(juce::roundToInt(v * float(steps - 1))) / float(steps - 1);
                param->setValue(v);
            }
        };
        randomise();
        juce::MemoryBlock state;
        plugin->getStateInformation(state);
        std::vector<float> saved;
        for (auto* param : own) saved.push_back(param->getValue());
        randomise();
        plugin->setStateInformation(state.getData(), int(state.getSize()));
        int mismatches = 0;
        for (size_t i = 0; i < own.size(); ++i)
            if (std::abs(own[i]->getValue() - saved[i]) > 1.0e-4f) ++mismatches;
        check(mismatches == 0, juce::String(mismatches) + " parameters not restored");

        plugin->releaseResources();
        // Mono in, stereo out.
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add(juce::AudioChannelSet::mono());
        mono.outputBuses.add(juce::AudioChannelSet::stereo());
        if (plugin->setBusesLayout(mono)) {
            plugin->prepareToPlay(rate, 512);
            const auto r = runFor(*plugin, 1.0, 512, rate, rng, true, false);
            check(r.finite && r.peak <= 1.0f, "mono input");
            plugin->releaseResources();
        } else {
            check(false, "mono input layout refused");
        }
    }
    std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
