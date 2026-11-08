#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Presets.h"

namespace {

// The DAW's view: each module's knobs in a group, every value as text with its unit, and
// typing a value back ("40 m", "-6 dB") finds the knob position that shows it.
juce::String groupOf(const std::string& id) {
    static const std::vector<std::pair<std::vector<std::string>, const char*>> groups = {
        {{"distance", "src_depth", "lis_depth", "water_depth", "sway"}, "SOURCE"},
        {{"path_on", "bottom", "waves", "reflections"}, "PATHS"},
        {{"echo_on", "echo_wall", "echo_dist", "echo_fb", "echo_level"}, "ECHO"},
        {{"water_on", "space", "size", "decay", "tone", "water_level"}, "REVERB"},
        {{"ear", "ear_amount", "clarity", "width"}, "LISTENER"},
        {{"eq_lowcut", "eq_low", "eq_mid_freq", "eq_mid", "eq_high"}, "EQ"}};
    for (const auto& [ids, name] : groups)
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) return name;
    return "GLOBAL";
}

float valueFromText(const std::function<std::string(float)>& text, const juce::String& typed) {
    const double wanted = typed.retainCharacters("+-.0123456789").getDoubleValue();
    float best = 0.0f;
    double bestDistance = 1.0e30;
    for (int i = 0; i <= 1000; ++i) {
        const float t = float(i) / 1000.0f;
        const double shown = juce::String(text(t)).retainCharacters("+-.0123456789").getDoubleValue();
        if (std::abs(shown - wanted) < bestDistance) bestDistance = std::abs(shown - wanted), best = t;
    }
    return best;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
    using namespace fathom::params;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::unique_ptr<juce::AudioProcessorParameterGroup> group;
    int groups = 0;
    for (const Spec& s : all()) {
        const auto name = groupOf(s.id);
        if (group == nullptr || group->getName() != name) {
            if (group != nullptr) layout.add(std::move(group));
            group = std::make_unique<juce::AudioProcessorParameterGroup>(name.toLowerCase() + juce::String(++groups), name, " | ");
        }
        const juce::ParameterID id{s.id.c_str(), 1};
        switch (s.kind) {
            case Kind::Float: {
                auto text = s.text;
                auto attributes = juce::AudioParameterFloatAttributes().withStringFromValueFunction([text](float v, int) {
                    return text ? juce::String::fromUTF8(text(v).c_str()) : juce::String(v, 2);
                });
                if (text) attributes = attributes.withValueFromStringFunction([text](const juce::String& typed) { return valueFromText(text, typed); });
                group->addChild(std::make_unique<juce::AudioParameterFloat>(id, juce::String(s.name), juce::NormalisableRange<float>(0.0f, 1.0f),
                                                                            s.defaultValue, attributes));
                break;
            }
            case Kind::Bool:
                group->addChild(std::make_unique<juce::AudioParameterBool>(id, juce::String(s.name), s.defaultValue > 0.5f));
                break;
            case Kind::Choice: {
                juce::StringArray choices;
                for (const auto& c : s.choices) choices.add(c);
                group->addChild(std::make_unique<juce::AudioParameterChoice>(id, juce::String(s.name), choices, int(s.defaultValue)));
                break;
            }
        }
    }
    if (group != nullptr) layout.add(std::move(group));
    return layout;
}

// Version 2 (v0.2.1): DRY became a parallel dry on top of SUBMERGE (it used to scale the
// crossfade's dry side, 0 dB by default). Older sessions get it off: they sound the same.
constexpr int kStateVersion = 2;

}  // namespace

FathomProcessor::FathomProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "FATHOM", createLayout()) {
    for (const auto& s : fathom::params::all()) raw_.push_back(apvts.getRawParameterValue(s.id.c_str()));
    values_.resize(raw_.size());
    applyPreset(0, false);  // a new instance starts on MASTER / Sous l'eau: ready for a master bus
}

bool FathomProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    return out == juce::AudioChannelSet::stereo() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void FathomProcessor::prepareToPlay(double sampleRate, int) {
    engine.prepare(float(sampleRate));
    setLatencySamples(0);
}

void FathomProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const auto start = juce::Time::getHighResolutionTicks();
    const int numSamples = buffer.getNumSamples();
    if (getTotalNumInputChannels() == 1) buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    for (size_t i = 0; i < raw_.size(); ++i) values_[i] = raw_[i]->load(std::memory_order_relaxed);
    engine.process(values_.data(), buffer.getWritePointer(0), buffer.getWritePointer(1), numSamples);

    const double elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    const double budget = numSamples / std::max(1.0, getSampleRate());
    cpuLoad.store(cpuLoad.load(std::memory_order_relaxed) * 0.95f + float(elapsed / budget) * 0.05f, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* FathomProcessor::createEditor() { return new FathomEditor(*this); }

void FathomProcessor::resetEngine() {
    suspendProcessing(true);  // waits for the audio callback, then keeps it out
    engine.reset();
    suspendProcessing(false);
}

int FathomProcessor::getNumPrograms() { return int(fathom::presets::all().size()); }

const juce::String FathomProcessor::getProgramName(int index) {
    const auto& list = fathom::presets::all();
    if (index < 0 || index >= int(list.size())) return {};
    return juce::String::fromUTF8(list[size_t(index)].category) + " / " + juce::String::fromUTF8(list[size_t(index)].name);
}

// Every parameter goes to the preset's value (its default where the preset says nothing).
void FathomProcessor::applyPreset(int index, bool flush) {
    const auto& list = fathom::presets::all();
    if (index < 0 || index >= int(list.size())) return;
    const auto values = fathom::presets::valuesOf(list[size_t(index)]);
    const auto& specs = fathom::params::all();
    for (size_t i = 0; i < specs.size(); ++i)
        if (auto* param = apvts.getParameter(specs[i].id.c_str())) param->setValueNotifyingHost(param->convertTo0to1(values[i]));
    if (flush) resetEngine();  // the previous preset's tails do not leak into this one
    currentPreset_ = index;
    juce::MessageManager::callAsync([safe = juce::WeakReference<FathomProcessor>(this)] {
        if (safe != nullptr && safe->onPresetChanged) safe->onPresetChanged();
    });
}

void FathomProcessor::resetAll() {
    for (auto* param : getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(param)) p->setValueNotifyingHost(p->getDefaultValue());
    resetEngine();
    currentPreset_ = -1;
    juce::MessageManager::callAsync([safe = juce::WeakReference<FathomProcessor>(this)] {
        if (safe != nullptr && safe->onPresetChanged) safe->onPresetChanged();
    });
}

void FathomProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    state.setProperty("version", kStateVersion, nullptr);
    state.setProperty("preset", currentPreset_, nullptr);
    state.setProperty("editorScale", editorScale, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void FathomProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    if (int(state.getProperty("version", 1)) < 2) {
        auto dry = state.getChildWithProperty("id", "dry");
        if (dry.isValid()) dry.setProperty("value", 0.0f, nullptr);
    }
    currentPreset_ = int(state.getProperty("preset", -1));
    editorScale = double(state.getProperty("editorScale", 0.0));
    apvts.replaceState(state);
    juce::MessageManager::callAsync([safe = juce::WeakReference<FathomProcessor>(this)] {
        if (safe != nullptr && safe->onPresetChanged) safe->onPresetChanged();
    });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FathomProcessor(); }
