#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine.h"

class FathomProcessor : public juce::AudioProcessor {
public:
    FathomProcessor();

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 24.0; }

    // Factory presets (Presets.h) are the host's programs too.
    int getNumPrograms() override;
    int getCurrentProgram() override { return std::max(0, currentPreset_); }
    int currentPresetOrInit() const { return currentPreset_; }  // -1 = INIT
    void setCurrentProgram(int index) override { applyPreset(index); }
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void applyPreset(int index, bool flush = true);
    void resetAll();
    std::function<void()> onPresetChanged;  // message thread, set by the editor

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    double editorScale = 0.0;  // the window's zoom, saved with the project (0 = not set)

    void resetEngine();
    juce::AudioProcessorValueTreeState apvts;
    fathom::Engine engine;
    std::atomic<float> cpuLoad{0.0f};

private:
    std::vector<std::atomic<float>*> raw_;
    std::vector<float> values_;
    int currentPreset_ = 0;
    std::atomic<bool> resetPending_{false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(FathomProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FathomProcessor)
};
