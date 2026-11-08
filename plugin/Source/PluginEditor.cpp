#include "PluginEditor.h"

#include <BinaryData.h>

#include "Presets.h"

namespace {

constexpr int kPageWidth = 1280, kPageHeight = 800;  // the page's layout size

std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url) {
    const auto make = [](const char* data, int size, const char* mime) {
        const auto* bytes = reinterpret_cast<const std::byte*>(data);
        return juce::WebBrowserComponent::Resource{std::vector<std::byte>(bytes, bytes + size), mime};
    };
    const auto path = url.upToFirstOccurrenceOf("?", false, false);
    if (path == "/" || path == "/index.html") return make(BinaryData::ui_html, BinaryData::ui_htmlSize, "text/html");
    if (path == "/juce/index.js") return make(BinaryData::index_js, BinaryData::index_jsSize, "text/javascript");
    if (path == "/juce/check_native_interop.js")
        return make(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize, "text/javascript");
    return std::nullopt;
}

juce::String utf8(const char* s) { return juce::String::fromUTF8(s); }

}  // namespace

SiltEditor::Relays SiltEditor::createRelays() {
    using namespace silt::params;
    Relays r;
    for (const Spec& s : all()) {
        switch (s.kind) {
            case Kind::Float: r.sliders.push_back(std::make_unique<juce::WebSliderRelay>(s.id.c_str())); break;
            case Kind::Bool: r.toggles.push_back(std::make_unique<juce::WebToggleButtonRelay>(s.id.c_str())); break;
            case Kind::Choice: r.combos.push_back(std::make_unique<juce::WebComboBoxRelay>(s.id.c_str())); break;
        }
    }
    return r;
}

juce::WebBrowserComponent::Options SiltEditor::createOptions(Relays& relays) {
    auto options = juce::WebBrowserComponent::Options{}
                       .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
                       .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}.withUserDataFolder(
                           juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SILT")))
                       .withNativeIntegrationEnabled()
                       .withResourceProvider(getResource)
                       .withNativeFunction("getPresets", [this](const juce::Array<juce::var>&, auto complete) {
                           juce::Array<juce::var> list;
                           for (const auto& p : silt::presets::all()) {
                               auto* item = new juce::DynamicObject();
                               item->setProperty("category", utf8(p.category));
                               item->setProperty("name", utf8(p.name));
                               item->setProperty("note", utf8(p.note));
                               list.add(juce::var(item));
                           }
                           auto* result = new juce::DynamicObject();
                           result->setProperty("list", list);
                           result->setProperty("current", processor_.currentPresetOrInit());
                           complete(juce::var(result));
                       })
                       .withNativeFunction("loadPreset", [this](const juce::Array<juce::var>& args, auto complete) {
                           if (!args.isEmpty()) processor_.applyPreset(int(args[0]));
                           complete(juce::var());
                       })
                       .withNativeFunction("resetAll", [this](const juce::Array<juce::var>&, auto complete) {
                           processor_.resetAll();
                           complete(juce::var());
                       })
                       // The page is up: send it every label once.
                       .withNativeFunction("ready", [this](const juce::Array<juce::var>&, auto complete) {
                           pageReady_ = true;
                           sendTexts(true);
                           sendEarCurve();
                           complete(juce::var());
                       });
    for (auto& s : relays.sliders) options = options.withOptionsFrom(*s);
    for (auto& t : relays.toggles) options = options.withOptionsFrom(*t);
    for (auto& c : relays.combos) options = options.withOptionsFrom(*c);
    return options;
}

SiltEditor::SiltEditor(SiltProcessor& p)
    : AudioProcessorEditor(p), processor_(p), relays_(createRelays()), webView_(createOptions(relays_)) {
    using namespace silt::params;
    size_t si = 0, ti = 0, ci = 0;
    for (const Spec& s : all()) {
        auto& param = *p.apvts.getParameter(s.id.c_str());
        switch (s.kind) {
            case Kind::Float: sliderAttachments_.push_back(std::make_unique<juce::WebSliderParameterAttachment>(param, *relays_.sliders[si++], nullptr)); break;
            case Kind::Bool: toggleAttachments_.push_back(std::make_unique<juce::WebToggleButtonParameterAttachment>(param, *relays_.toggles[ti++], nullptr)); break;
            case Kind::Choice: comboAttachments_.push_back(std::make_unique<juce::WebComboBoxParameterAttachment>(param, *relays_.combos[ci++], nullptr)); break;
        }
    }
    shownValues_.assign(all().size(), -1.0f);
    earModel_.prepare(48000.0f);

    processor_.onPresetChanged = [this] { webView_.emitEventIfBrowserIsVisible("silt_preset", processor_.currentPresetOrInit()); };

    addAndMakeVisible(webView_);
    webView_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    double scale = processor_.editorScale;
    if (scale <= 0.0) scale = juce::PropertiesFile(settingsOptions()).getDoubleValue("editorScale", 0.85);
    scale = juce::jlimit(0.5, 1.5, scale);
    setResizable(true, true);
    setResizeLimits(kPageWidth / 2, kPageHeight / 2, kPageWidth * 3 / 2, kPageHeight * 3 / 2);
    getConstrainer()->setFixedAspectRatio(double(kPageWidth) / kPageHeight);
    setSize(int(kPageWidth * scale), int(kPageHeight * scale));
    startTimerHz(30);
}

SiltEditor::~SiltEditor() {
    stopTimer();
    if (processor_.editorScale > 0.0) {
        juce::PropertiesFile settings(settingsOptions());
        settings.setValue("editorScale", processor_.editorScale);
        settings.saveIfNeeded();
    }
    processor_.onPresetChanged = nullptr;
}

void SiltEditor::resized() {
    webView_.setBounds(getLocalBounds());
    processor_.editorScale = double(getWidth()) / kPageWidth;
}

juce::PropertiesFile::Options SiltEditor::settingsOptions() {
    juce::PropertiesFile::Options o;
    o.applicationName = "settings";
    o.folderName = "SILT";
    o.filenameSuffix = ".xml";
    o.osxLibrarySubFolder = "Application Support";
    o.millisecondsBeforeSaving = -1;
    return o;
}

// The DAW's label of every parameter whose value moved (all of them on request).
void SiltEditor::sendTexts(bool all) {
    const auto& specs = silt::params::all();
    auto* texts = new juce::DynamicObject();
    bool any = false, earMoved = false;
    for (size_t i = 0; i < specs.size(); ++i) {
        auto* param = processor_.apvts.getParameter(specs[i].id.c_str());
        const float v = param->getValue();
        if (!all && v == shownValues_[i]) continue;
        shownValues_[i] = v;
        texts->setProperty(juce::Identifier(specs[i].id.c_str()), param->getCurrentValueAsText());
        any = true;
        if (specs[i].id == "ear" || specs[i].id == "ear_amount" || specs[i].id == "clarity" || specs[i].id == "submerge" || specs[i].id == "muffle") earMoved = true;
    }
    if (any) webView_.emitEventIfBrowserIsVisible("silt_text", juce::var(texts));
    else delete texts;
    if (earMoved && !all) sendEarCurve();
}

// The listener's response, 64 points from 20 Hz to 20 kHz, in dB (level match included).
void SiltEditor::sendEarCurve() {
    const auto value = [&](const char* id) { return processor_.apvts.getRawParameterValue(id)->load(); };
    silt::Ear::Settings s;
    s.mode = int(std::lround(value("ear")));
    s.amount = value("ear_amount") * value("submerge");
    s.clarity = value("clarity");
    s.width = 1.0f;
    earModel_.set(s);
    earModel_.update();
    // MUFFLE drawn on top of the ear: the same three filters as silt::Muffle.
    const float muffle = value("muffle"), corner = silt::muffleCornerHz(muffle);
    silt::Svf a, b, boom;
    a.set(silt::Svf::Type::Lowpass, corner, 0.54f, 0.0f, 48000.0f);
    b.set(silt::Svf::Type::Lowpass, corner, 1.0f + 0.6f * muffle, 0.0f, 48000.0f);
    boom.set(silt::Svf::Type::Bell, std::max(40.0f, corner * 0.55f), 0.9f, 4.0f * muffle, 48000.0f);
    juce::Array<juce::var> curve;
    for (int i = 0; i < 64; ++i) {
        const float hz = 20.0f * std::pow(1000.0f, float(i) / 63.0f);
        float db = earModel_.responseDb(hz);
        if (corner > 0.0f) db += silt::gainToDb(a.magnitude(hz, 48000.0f) * b.magnitude(hz, 48000.0f) * boom.magnitude(hz, 48000.0f));
        curve.add(db);
    }
    webView_.emitEventIfBrowserIsVisible("silt_ear", curve);
}

void SiltEditor::timerCallback() {
    if (!pageReady_) return;
    sendTexts(false);
    const auto& engine = processor_.engine;
    auto* live = new juce::DynamicObject();
    live->setProperty("in", engine.inLevel());
    live->setProperty("out", engine.outLevel());
    juce::Array<juce::var> bands;
    for (int b = 0; b < 3; ++b) bands.add(engine.band(b));
    live->setProperty("bands", bands);
    // Arrivals: [ms, gain, surface bounces, bottom bounces] for every audible path.
    juce::Array<juce::var> arrivals;
    const auto& paths = engine.paths();
    for (int i = 0; i < silt::MultiPath::kArrivalsShown; ++i) {
        const auto& a = paths.arrival(i);
        const float g = a.gain.load(std::memory_order_relaxed);
        if (std::abs(g) < 0.003f) continue;
        juce::Array<juce::var> item;
        item.add(a.ms.load(std::memory_order_relaxed)), item.add(g);
        item.add(a.surface.load(std::memory_order_relaxed)), item.add(a.bottom.load(std::memory_order_relaxed));
        arrivals.add(item);
    }
    live->setProperty("arrivals", arrivals);
    live->setProperty("echoMs", engine.echoMs());

    live->setProperty("absorptionHz", paths.absorptionHz());
    live->setProperty("modalHz", paths.modalHz());
    if (++ticks_ % 10 == 0) {
        live->setProperty("cpu", processor_.cpuLoad.load() * 100.0f);
        live->setProperty("sampleRate", processor_.getSampleRate());
    }
    webView_.emitEventIfBrowserIsVisible("silt_live", juce::var(live));
}
