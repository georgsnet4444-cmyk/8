#include "PluginEditor.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
const juce::Colour background { 0xff0b1015 };
const juce::Colour surface { 0xff121a22 };
const juce::Colour surfaceRaised { 0xff18232d };
const juce::Colour border { 0xff2b3b48 };
const juce::Colour text { 0xffe9f2f5 };
const juce::Colour muted { 0xff8499a6 };
const juce::Colour accent { 0xff65e0bf };
const juce::Colour warm { 0xffffb66b };
const juce::Colour danger { 0xffff786f };

constexpr float spectrumFloorDb = -90.0f;

float normalizedMeter(float db)
{
    return juce::jlimit(0.0f, 1.0f, (db + 48.0f) / 48.0f);
}

juce::String formatFrequency(float frequency)
{
    if (frequency >= 1000.0f)
        return juce::String(frequency / 1000.0f, 2) + " kHz";
    return juce::String(frequency, 1) + " Hz";
}

juce::String formatDb(float value, int decimals = 2)
{
    return (value >= 0.0f ? "+" : "") + juce::String(value, decimals);
}

juce::String noteName(float frequency)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto midi = 69.0f + 12.0f * std::log2(juce::jmax(1.0f, frequency) / 440.0f);
    const auto nearest = juce::roundToInt(midi);
    const auto cents = juce::roundToInt((midi - static_cast<float>(nearest)) * 100.0f);
    const auto index = ((nearest % 12) + 12) % 12;
    const auto octave = static_cast<int>(std::floor(static_cast<float>(nearest) / 12.0f)) - 1;
    return juce::String(names[index]) + juce::String(octave) + " " + (cents >= 0 ? "+" : "")
           + juce::String(cents) + "c";
}
}

NorthstarMasteringAudioProcessorEditor::NorthstarMasteringAudioProcessorEditor(
    NorthstarMasteringAudioProcessor& processorToUse)
    : AudioProcessorEditor(&processorToUse),
      processor(processorToUse),
      eqGraph(processorToUse)
{
    setSize(1120, 720);
    setResizable(true, true);
    setResizeLimits(900, 720, 1600, 1000);

    brandLabel.setText("NORTHSTAR", juce::dontSendNotification);
    brandLabel.setFont(juce::Font(juce::FontOptions(21.0f, juce::Font::bold)));
    brandLabel.setColour(juce::Label::textColourId, text);
    addAndMakeVisible(brandLabel);
    subtitleLabel.setText("MASTERING SYSTEM  /  10 SECOND LEARN", juce::dontSendNotification);
    subtitleLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    subtitleLabel.setColour(juce::Label::textColourId, muted);
    addAndMakeVisible(subtitleLabel);

    for (auto* tab : { &eqTab, &loudnessTab, &saturationTab, &stereoTab, &compressorTab })
    {
        styleButton(*tab, false);
        tab->setClickingTogglesState(false);
        addAndMakeVisible(*tab);
    }
    eqTab.onClick = [this] { setPage(Page::eq); };
    loudnessTab.onClick = [this] { setPage(Page::loudness); };
    saturationTab.onClick = [this] { setPage(Page::saturation); };
    stereoTab.onClick = [this] { setPage(Page::stereo); };
    compressorTab.onClick = [this] { setPage(Page::compressor); };

    styleButton(bypassButton);
    bypassAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "bypass", bypassButton);
    addAndMakeVisible(bypassButton);

    analyzeButton.setColour(juce::TextButton::buttonColourId, accent);
    analyzeButton.setColour(juce::TextButton::textColourOffId, background);
    analyzeButton.setColour(juce::TextButton::buttonOnColourId, accent);
    analyzeButton.onClick = [this] { processor.requestAnalysis(); };
    addAndMakeVisible(analyzeButton);
    styleButton(learnButton, false);
    learnButton.setButtonText("LEARN 10 SEC");
    learnButton.onClick = [this] { processor.requestAnalysis(); };
    addAndMakeVisible(learnButton);

    // Full-disable button of every tab (bound to the module's enable parameter).
    for (auto* power : { &eqPower, &loudnessPower, &saturationPower, &stereoPower, &compressorPower })
    {
        stylePowerButton(*power);
        addAndMakeVisible(*power);
    }
    eqPowerAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "eqEnabled", eqPower);
    loudnessPowerAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "loudnessEnabled", loudnessPower);
    saturationPowerAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "saturationEnabled", saturationPower);
    stereoPowerAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "stereoEnabled", stereoPower);
    compressorPowerAttachment = std::make_unique<ButtonAttachment>(
        processor.parameters, "compressorEnabled", compressorPower);

    const auto styleLabel = [this](juce::Label& target, const juce::String& value)
    {
        target.setText(value, juce::dontSendNotification);
        target.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        target.setColour(juce::Label::textColourId, muted);
        addAndMakeVisible(target);
    };
    styleLabel(inputLabel, "INPUT  /  LUFS");
    styleLabel(outputLabel, "OUTPUT  /  LUFS");
    styleLabel(reductionLabel, "GAIN REDUCTION");
    styleLabel(statusLabel, "READY");

    addAndMakeVisible(meter);
    addAndMakeVisible(spectrum);
    addAndMakeVisible(eqGraph);
    eqGraph.onBandSelected = [this](int band)
    {
        selectedEQBand = band;
        updateEQControls();
    };

    eqModeSelector.addItem("AUTO", 1);
    eqModeSelector.addItem("MANUAL", 2);
    eqModeSelector.setTooltip("Automatic uses the 10 second learn result. Manual keeps your node positions.");
    eqModeAttachment = std::make_unique<ComboAttachment>(processor.parameters, "eqMode", eqModeSelector);
    addAndMakeVisible(eqModeSelector);

    eqPhaseLabel.setText("LINEAR PHASE  -  NO PHASE SHIFT", juce::dontSendNotification);
    eqPhaseLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    eqPhaseLabel.setColour(juce::Label::textColourId, accent);
    eqPhaseLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(eqPhaseLabel);

    styleToggle(eqDynamicButton);
    addAndMakeVisible(eqDynamicButton);
    eqDynamicButton.setToggleState(false, juce::dontSendNotification);
    eqDynamicButton.onClick = [this]
    {
        processor.setEQDynamic(selectedEQBand, eqDynamicButton.getToggleState());
    };
    eqAutoButton.setColour(juce::TextButton::buttonColourId, surfaceRaised);
    eqAutoButton.setColour(juce::TextButton::textColourOffId, text);
    eqAutoButton.onClick = [this] { processor.resetEQToAuto(); updateEQControls(); };
    addAndMakeVisible(eqAutoButton);

    styleSlider(eqFrequencySlider, " Hz");
    styleSlider(eqGainSlider, " dB");
    styleSlider(eqQSlider, " Q");
    eqFrequencySlider.setNumDecimalPlacesToDisplay(1);
    eqGainSlider.setNumDecimalPlacesToDisplay(2);
    styleControlLabel(eqControlLabels[0], "FREQUENCY");
    styleControlLabel(eqControlLabels[1], "GAIN");
    styleControlLabel(eqControlLabels[2], "Q");
    eqFrequencySlider.setRange(20.0, 20000.0, 0.01);
    eqFrequencySlider.setSkewFactorFromMidPoint(800.0);
    eqGainSlider.setRange(-18.0, 18.0, 0.01);
    eqQSlider.setRange(0.1, 12.0, 0.01);
    eqFrequencySlider.onValueChange = [this]
    {
        if (!updatingEQControls) processor.setEQFrequency(selectedEQBand,
                                                           static_cast<float>(eqFrequencySlider.getValue()));
        eqGraph.repaint();
    };
    eqGainSlider.onValueChange = [this]
    {
        if (!updatingEQControls) processor.setEQGain(selectedEQBand,
                                                      static_cast<float>(eqGainSlider.getValue()));
        eqGraph.repaint();
    };
    eqQSlider.onValueChange = [this]
    {
        if (!updatingEQControls) processor.setEQQ(selectedEQBand,
                                                   static_cast<float>(eqQSlider.getValue()));
        eqGraph.repaint();
    };
    addAndMakeVisible(eqFrequencySlider);
    addAndMakeVisible(eqGainSlider);
    addAndMakeVisible(eqQSlider);
    addAndMakeVisible(eqBandLabel);
    eqBandLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    eqBandLabel.setColour(juce::Label::textColourId, accent);

    styleSlider(targetSlider, " LUFS");
    styleSlider(volumeSlider, " dB");
    targetSlider.setRange(-24.0, -8.0, 0.1);
    volumeSlider.setRange(-24.0, 24.0, 0.01);
    styleControlLabel(loudnessControlLabels[0], "TARGET LUFS");
    styleControlLabel(loudnessControlLabels[1], "CLEAN VOLUME");
    targetAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "targetLufs", targetSlider);
    volumeAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "volume", volumeSlider);
    addAndMakeVisible(targetSlider);
    addAndMakeVisible(volumeSlider);

    styleSlider(saturationMixSlider, " %");
    styleControlLabel(saturationControlLabel, "MIX");
    saturationMixSlider.setRange(0.0, 100.0, 0.1);
    saturationMixAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "saturationMix", saturationMixSlider);
    saturationPresetSelector.addItem("WARM", 1);
    saturationPresetSelector.addItem("COLD / AIRY", 2);
    saturationPresetSelector.addItem("DISTORTION", 3);
    saturationPresetSelector.addItem("TUBE", 4);
    saturationPresetAttachment = std::make_unique<ComboAttachment>(
        processor.parameters, "saturationPreset", saturationPresetSelector);
    addAndMakeVisible(saturationMixSlider);
    addAndMakeVisible(saturationPresetSelector);

    for (int index = 0; index < 3; ++index)
    {
        styleSlider(crossoverSliders[static_cast<size_t>(index)], " Hz");
        styleControlLabel(crossoverLabels[static_cast<size_t>(index)],
                          "XOVER " + juce::String(index + 1));
        crossoverSliders[static_cast<size_t>(index)].setRange(40.0, 18000.0, 0.1);
        crossoverSliders[static_cast<size_t>(index)].setSkewFactorFromMidPoint(800.0);
        crossoverAttachments[static_cast<size_t>(index)] = std::make_unique<SliderAttachment>(
            processor.parameters, "stereoXover" + juce::String(index + 1),
            crossoverSliders[static_cast<size_t>(index)]);
    }
    for (int index = 0; index < 4; ++index)
    {
        styleSlider(widthSliders[static_cast<size_t>(index)], " %");
        styleControlLabel(widthLabels[static_cast<size_t>(index)],
                          "BAND " + juce::String(index + 1) + " WIDTH");
        widthSliders[static_cast<size_t>(index)].setRange(0.0, 200.0, 0.1);
        widthAttachments[static_cast<size_t>(index)] = std::make_unique<SliderAttachment>(
            processor.parameters, "stereoWidth" + juce::String(index + 1),
            widthSliders[static_cast<size_t>(index)]);
        stereoPresetSelectors[static_cast<size_t>(index)].addItem("NEUTRAL", 1);
        stereoPresetSelectors[static_cast<size_t>(index)].addItem("WIDE / AIRY", 2);
        stereoPresetSelectors[static_cast<size_t>(index)].addItem("NARROW / FOCUS", 3);
        stereoPresetSelectors[static_cast<size_t>(index)].addItem("WIDE / TUBE", 4);
        stereoPresetAttachments[static_cast<size_t>(index)] = std::make_unique<ComboAttachment>(
            processor.parameters, "stereoPreset" + juce::String(index + 1),
            stereoPresetSelectors[static_cast<size_t>(index)]);
        if (index < 3)
            addAndMakeVisible(crossoverSliders[static_cast<size_t>(index)]);
        addAndMakeVisible(widthSliders[static_cast<size_t>(index)]);
        addAndMakeVisible(stereoPresetSelectors[static_cast<size_t>(index)]);
    }

    const std::array<std::pair<juce::Slider*, const char*>, 6> compControls {
        std::pair { &compAttackSlider, " ms" }, std::pair { &compReleaseSlider, " ms" },
        std::pair { &compRatioSlider, " :1" }, std::pair { &compInputSlider, " dB" },
        std::pair { &compOutputSlider, " dB" }, std::pair { &compThresholdSlider, " dB" }
    };
    for (auto [slider, suffix] : compControls)
    {
        styleSlider(*slider, suffix);
        addAndMakeVisible(*slider);
    }
    const std::array<const char*, 6> compressorLabels {
        "ATTACK", "RELEASE", "RATIO", "INPUT GAIN", "MAKEUP GAIN", "THRESHOLD"
    };
    for (size_t index = 0; index < compressorLabels.size(); ++index)
        styleControlLabel(compressorControlLabels[index], compressorLabels[index]);
    compAttackSlider.setRange(1.0, 200.0, 0.1);
    compReleaseSlider.setRange(20.0, 2000.0, 0.1);
    compRatioSlider.setRange(1.0, 20.0, 0.01);
    compInputSlider.setRange(-24.0, 24.0, 0.01);
    compOutputSlider.setRange(-24.0, 24.0, 0.01);
    compThresholdSlider.setRange(-48.0, 0.0, 0.1);
    compAttackAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compAttack", compAttackSlider);
    compReleaseAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compRelease", compReleaseSlider);
    compRatioAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compRatio", compRatioSlider);
    compInputAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compInput", compInputSlider);
    compOutputAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compOutput", compOutputSlider);
    compThresholdAttachment = std::make_unique<SliderAttachment>(
        processor.parameters, "compThreshold", compThresholdSlider);

    setPage(Page::eq);
    updateEQControls();
    startTimerHz(30);
}

void NorthstarMasteringAudioProcessorEditor::styleSlider(juce::Slider& slider, const juce::String& suffix)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 96, 24);
    slider.setTextValueSuffix(suffix);
    if (suffix.contains("Hz") || suffix.contains("%") || suffix.contains("ms"))
        slider.setNumDecimalPlacesToDisplay(0);
    else if (suffix.contains(":1"))
        slider.setNumDecimalPlacesToDisplay(1);
    else if (suffix.contains(" Q"))
        slider.setNumDecimalPlacesToDisplay(2);
    else
        slider.setNumDecimalPlacesToDisplay(1);
    slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, border);
    slider.setColour(juce::Slider::thumbColourId, text);
    slider.setColour(juce::Slider::textBoxTextColourId, text);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, surfaceRaised);
    slider.setColour(juce::Slider::textBoxOutlineColourId, border);
}

void NorthstarMasteringAudioProcessorEditor::styleControlLabel(
    juce::Label& label, const juce::String& value)
{
    label.setText(value, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    label.setColour(juce::Label::textColourId, muted);
    label.setJustificationType(juce::Justification::centred);
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

void NorthstarMasteringAudioProcessorEditor::styleButton(juce::TextButton& button, bool toggle)
{
    button.setClickingTogglesState(toggle);
    button.setColour(juce::TextButton::buttonColourId, surfaceRaised);
    button.setColour(juce::TextButton::buttonOnColourId, accent);
    button.setColour(juce::TextButton::textColourOffId, text);
    button.setColour(juce::TextButton::textColourOnId, background);
}

void NorthstarMasteringAudioProcessorEditor::stylePowerButton(juce::TextButton& button)
{
    button.setClickingTogglesState(true);
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4a2626));
    button.setColour(juce::TextButton::buttonOnColourId, accent);
    button.setColour(juce::TextButton::textColourOffId, danger);
    button.setColour(juce::TextButton::textColourOnId, background);
    button.setTooltip("Fully enable / disable this section");
}

void NorthstarMasteringAudioProcessorEditor::styleToggle(juce::ToggleButton& button)
{
    button.setClickingTogglesState(true);
    button.setColour(juce::ToggleButton::textColourId, text);
    button.setColour(juce::ToggleButton::tickColourId, accent);
    button.setColour(juce::ToggleButton::tickDisabledColourId, border);
}

void NorthstarMasteringAudioProcessorEditor::setPage(Page nextPage)
{
    page = nextPage;
    const auto setTab = [](juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId, active ? accent : surfaceRaised);
        button.setColour(juce::TextButton::textColourOffId, active ? background : text);
    };
    setTab(eqTab, page == Page::eq);
    setTab(loudnessTab, page == Page::loudness);
    setTab(saturationTab, page == Page::saturation);
    setTab(stereoTab, page == Page::stereo);
    setTab(compressorTab, page == Page::compressor);

    const auto eqVisible = page == Page::eq;
    const auto loudnessVisible = page == Page::loudness;
    const auto satVisible = page == Page::saturation;
    const auto stereoVisible = page == Page::stereo;
    const auto compVisible = page == Page::compressor;
    const auto setComponentsVisible = [](bool visible,
                                         std::initializer_list<juce::Component*> components)
    {
        for (auto* component : components)
            component->setVisible(visible);
    };
    setComponentsVisible(eqVisible, { &eqGraph, &eqModeSelector, &eqDynamicButton, &eqAutoButton,
                                      &eqFrequencySlider, &eqGainSlider, &eqQSlider,
                                      &eqBandLabel, &eqPhaseLabel, &eqPower });
    for (auto& label : eqControlLabels)
        label.setVisible(eqVisible);
    setComponentsVisible(loudnessVisible, { &targetSlider, &volumeSlider, &learnButton, &loudnessPower });
    for (auto& label : loudnessControlLabels)
        label.setVisible(loudnessVisible);
    setComponentsVisible(satVisible, { &saturationMixSlider, &saturationPresetSelector,
                                       &saturationControlLabel, &saturationPower });
    setComponentsVisible(stereoVisible, { &stereoPower, &crossoverSliders[0], &crossoverSliders[1],
                                          &crossoverSliders[2], &widthSliders[0],
                                          &widthSliders[1], &widthSliders[2],
                                          &widthSliders[3], &stereoPresetSelectors[0],
                                          &stereoPresetSelectors[1], &stereoPresetSelectors[2],
                                          &stereoPresetSelectors[3] });
    for (auto& label : crossoverLabels)
        label.setVisible(stereoVisible);
    for (auto& label : widthLabels)
        label.setVisible(stereoVisible);
    setComponentsVisible(compVisible, { &compressorPower, &compAttackSlider, &compReleaseSlider,
                                        &compRatioSlider, &compInputSlider,
                                        &compOutputSlider, &compThresholdSlider });
    for (auto& label : compressorControlLabels)
        label.setVisible(compVisible);
    resized();
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::updateEQControls()
{
    updatingEQControls = true;
    eqFrequencySlider.setValue(processor.getEQFrequency(selectedEQBand), juce::dontSendNotification);
    eqGainSlider.setValue(processor.getEQGain(selectedEQBand), juce::dontSendNotification);
    eqQSlider.setValue(processor.getEQQ(selectedEQBand), juce::dontSendNotification);
    eqDynamicButton.setToggleState(processor.getEQDynamic(selectedEQBand), juce::dontSendNotification);
    eqBandLabel.setText("NODE " + juce::String(selectedEQBand + 1) + " / 15",
                        juce::dontSendNotification);
    updatingEQControls = false;
    eqGraph.setSelectedBand(selectedEQBand);
}

void NorthstarMasteringAudioProcessorEditor::updateLabels()
{
    inputLabel.setText("INPUT  " + juce::String(processor.getLoudnessEstimate(), 1) + " LUFS",
                       juce::dontSendNotification);
    outputLabel.setText("OUTPUT  " + juce::String(processor.getOutputLoudness(), 1) + " LUFS",
                        juce::dontSendNotification);
    reductionLabel.setText("GAIN REDUCTION  -" + juce::String(processor.getGainReduction(), 1) + " dB",
                           juce::dontSendNotification);
    statusLabel.setText(processor.isAnalysisRunning()
                            ? "LEARNING  " + juce::String(static_cast<int>(
                                  processor.getAnalysisProgress() * 100.0f)) + "%"
                            : (processor.hasAnalysis() ? "AUTO PROFILE READY" : "READY"),
                        juce::dontSendNotification);
    analyzeButton.setButtonText(processor.isAnalysisRunning()
                                    ? "ANALYZING  " + juce::String(static_cast<int>(
                                          processor.getAnalysisProgress() * 100.0f)) + "%"
                                    : "ANALYZE 10 SEC");
}

// Mirrors every module's power state into the UI: the tab title gets an (OFF) tag,
// the power button changes label, and the section's controls are dimmed.
void NorthstarMasteringAudioProcessorEditor::updateModuleStates()
{
    const auto isOn = [this](const char* id)
    {
        return processor.parameters.getRawParameterValue(id)->load() > 0.5f;
    };
    const auto tabText = [](juce::TextButton& button, const juce::String& base, bool on)
    {
        const auto value = on ? base : base + "  (OFF)";
        if (button.getButtonText() != value)
            button.setButtonText(value);
    };
    const auto powerText = [](juce::TextButton& button, bool on)
    {
        const juce::String value = on ? "POWER  ON" : "POWER  OFF";
        if (button.getButtonText() != value)
            button.setButtonText(value);
    };
    const auto fade = [](bool on, std::initializer_list<juce::Component*> components)
    {
        const auto alpha = on ? 1.0f : 0.38f;
        for (auto* component : components)
            if (component->getAlpha() != alpha)
                component->setAlpha(alpha);
    };

    const auto eqOn = isOn("eqEnabled");
    const auto loudnessOn = isOn("loudnessEnabled");
    const auto saturationOn = isOn("saturationEnabled");
    const auto stereoOn = isOn("stereoEnabled");
    const auto compressorOn = isOn("compressorEnabled");

    tabText(eqTab, "EQUALIZATION", eqOn);
    tabText(loudnessTab, "LOUDNESS", loudnessOn);
    tabText(saturationTab, "SATURATION", saturationOn);
    tabText(stereoTab, "STEREO IMAGER", stereoOn);
    tabText(compressorTab, "OPTO COMPRESSOR", compressorOn);
    powerText(eqPower, eqOn);
    powerText(loudnessPower, loudnessOn);
    powerText(saturationPower, saturationOn);
    powerText(stereoPower, stereoOn);
    powerText(compressorPower, compressorOn);

    fade(eqOn, { &eqGraph, &eqModeSelector, &eqDynamicButton, &eqAutoButton, &eqFrequencySlider,
                 &eqGainSlider, &eqQSlider, &eqBandLabel, &eqPhaseLabel, &eqControlLabels[0],
                 &eqControlLabels[1], &eqControlLabels[2] });
    fade(loudnessOn, { &targetSlider, &volumeSlider, &learnButton, &loudnessControlLabels[0],
                       &loudnessControlLabels[1] });
    fade(saturationOn, { &saturationMixSlider, &saturationPresetSelector, &saturationControlLabel });
    for (size_t i = 0; i < 3; ++i)
        fade(stereoOn, { &crossoverSliders[i], &crossoverLabels[i] });
    for (size_t i = 0; i < 4; ++i)
        fade(stereoOn, { &widthSliders[i], &widthLabels[i], &stereoPresetSelectors[i] });
    fade(compressorOn, { &compAttackSlider, &compReleaseSlider, &compRatioSlider, &compInputSlider,
                         &compOutputSlider, &compThresholdSlider });
    for (auto& label : compressorControlLabels)
        fade(compressorOn, { &label });
}

void NorthstarMasteringAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(background);
    g.setColour(surface);
    g.fillRoundedRectangle(18.0f, 85.0f, static_cast<float>(getWidth() - 36),
                           static_cast<float>(getHeight() - 103), 12.0f);
    g.setColour(border);
    g.drawRoundedRectangle(18.0f, 85.0f, static_cast<float>(getWidth() - 36),
                           static_cast<float>(getHeight() - 103), 12.0f, 1.0f);
    g.setColour(muted);
    g.setFont(10.0f);
    g.drawText("REAL-TIME MASTERING / HOST AUTOMATION READY", 32, 98, 360, 18,
               juce::Justification::left);
    const auto footerTop = getHeight() - 142;
    g.setColour(border);
    g.drawHorizontalLine(footerTop - 10, 32.0f, static_cast<float>(getWidth() - 32));
    g.setColour(muted);
    g.drawText("OUTPUT LEVEL", 235, footerTop - 7, 170, 16, juce::Justification::left);
    g.setFont(9.0f);
    g.drawText("-48 dB", 235, footerTop + 29, 48, 14, juce::Justification::left);
    g.drawText("-24", 235 + static_cast<int>((getWidth() - 265) * 0.50f) - 18, footerTop + 29,
               36, 14, juce::Justification::centred);
    g.drawText("-12", 235 + static_cast<int>((getWidth() - 265) * 0.75f) - 18, footerTop + 29,
               36, 14, juce::Justification::centred);
    g.drawText("0 dB", getWidth() - 72, footerTop + 29, 40, 14,
               juce::Justification::right);
    g.drawText("LIVE INPUT SPECTRUM", 32, footerTop + 62, 220, 16,
               juce::Justification::left);

    const auto title = [&g](const juce::String& value, int width)
    {
        g.setColour(text);
        g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        g.drawText(value, 48, 132, width, 34, juce::Justification::left);
        g.setColour(muted);
        g.setFont(12.0f);
    };

    if (page == Page::eq)
    {
        g.setColour(muted);
        g.setFont(10.5f);
        g.drawText("Drag a node = frequency / gain (Shift = fine).  Mouse wheel over a node = bandwidth (Q): "
                   "wheel up widens, wheel down narrows.  Double click = reset gain.",
                   42, 122, getWidth() - 260, 18, juce::Justification::left);
    }
    else if (page == Page::loudness)
    {
        title("LOUDNESS ALIGNMENT", 500);
        g.drawText("Match a LUFS target, then trim the result with a transparent gain control.", 50, 172,
                   getWidth() - 100, 22, juce::Justification::left);
        g.drawText("The volume control is clean gain only - no saturation or clipping stage is added.", 50, 198,
                   getWidth() - 100, 22, juce::Justification::left);
    }
    else if (page == Page::saturation)
    {
        title("SATURATION", 500);
        g.drawText("Blend harmonics from zero to full wet. Presets change the drive character.", 50, 172,
                   getWidth() - 100, 22, juce::Justification::left);
    }
    else if (page == Page::stereo)
    {
        title("FOUR-BAND STEREO IMAGER", 600);
        g.drawText("Set the three crossover points and width/preset independently in each band.", 50, 172,
                   getWidth() - 100, 22, juce::Justification::left);
    }
    else
    {
        title("OPTO COMPRESSOR", 600);
        g.drawText("Manual attack, release, ratio, input, output and threshold with live gain-reduction metering.", 50, 172,
                   getWidth() - 100, 22, juce::Justification::left);
    }
}

void NorthstarMasteringAudioProcessorEditor::resized()
{
    const auto w = getWidth();
    const auto h = getHeight();
    brandLabel.setBounds(26, 16, 180, 28);
    subtitleLabel.setBounds(28, 44, 300, 18);
    analyzeButton.setBounds(w - 226, 18, 164, 32);
    bypassButton.setBounds(w - 54, 18, 42, 32);
    statusLabel.setBounds(w - 410, 47, 280, 18);

    const int tabY = 63;
    const int tabW = juce::jmax(120, (w - 52) / 5);
    juce::TextButton* tabs[] { &eqTab, &loudnessTab, &saturationTab, &stereoTab, &compressorTab };
    for (int i = 0; i < 5; ++i)
        tabs[i]->setBounds(26 + i * tabW, tabY, tabW - 5, 28);

    // Power button of the visible tab, same place on every page.
    const auto powerBounds = juce::Rectangle<int>(w - 190, 94, 158, 24);
    for (auto* power : { &eqPower, &loudnessPower, &saturationPower, &stereoPower, &compressorPower })
        power->setBounds(powerBounds);

    const auto footerTop = h - 142;
    inputLabel.setBounds(32, footerTop + 1, 190, 20);
    outputLabel.setBounds(32, footerTop + 23, 190, 20);
    reductionLabel.setBounds(32, footerTop + 45, 220, 20);
    meter.setBounds(235, footerTop + 7, w - 265, 22);
    spectrum.setBounds(32, footerTop + 80, w - 64, 42);

    // Bounds are assigned for every page on every resize. Visibility is managed
    // by setPage(); doing this here prevents controls from keeping a 0x0 bounds
    // rectangle after switching to another tab.
    const auto eqGraphWidth = juce::jmax(300, w - 330);
    eqGraph.setBounds(42, 148, eqGraphWidth, juce::jmax(260, footerTop - 16 - 148));
    eqModeSelector.setBounds(w - 268, 148, 224, 30);
    eqPhaseLabel.setBounds(w - 268, 184, 224, 16);
    eqBandLabel.setBounds(w - 268, 206, 224, 22);
    eqControlLabels[0].setBounds(w - 282, 230, 120, 18);
    eqControlLabels[1].setBounds(w - 150, 230, 120, 18);
    eqFrequencySlider.setBounds(w - 282, 248, 120, 112);
    eqGainSlider.setBounds(w - 150, 248, 120, 112);
    eqControlLabels[2].setBounds(w - 282, 368, 120, 18);
    eqQSlider.setBounds(w - 282, 386, 120, 112);
    eqDynamicButton.setBounds(w - 150, 372, 120, 28);
    eqAutoButton.setBounds(w - 150, 410, 120, 28);

    loudnessControlLabels[0].setBounds(120, 214, 200, 20);
    loudnessControlLabels[1].setBounds(390, 214, 200, 20);
    targetSlider.setBounds(120, 234, 200, 178);
    volumeSlider.setBounds(390, 234, 200, 178);
    learnButton.setBounds(w - 250, 245, 190, 40);

    saturationControlLabel.setBounds(w / 2 - 115, 202, 230, 20);
    saturationMixSlider.setBounds(w / 2 - 115, 222, 230, 220);
    saturationPresetSelector.setBounds(w / 2 - 130, 470, 260, 34);

    const auto stereoMargin = 70;
    const auto stereoGap = 14;
    const auto crossoverWidth = juce::jmax(106, (w - 2 * stereoMargin - 2 * stereoGap) / 3);
    const auto crossoverY = 250;
    const auto crossoverHeight = juce::jmax(96, juce::jmin(142, h - 570));
    for (int i = 0; i < 3; ++i)
    {
        const auto x = stereoMargin + i * (crossoverWidth + stereoGap);
        crossoverLabels[static_cast<size_t>(i)].setBounds(x, crossoverY - 20,
                                                            crossoverWidth, 18);
        crossoverSliders[static_cast<size_t>(i)].setBounds(x, crossoverY,
                                                             crossoverWidth, crossoverHeight);
    }
    const auto widthY = crossoverY + crossoverHeight + 32;
    const auto widthHeight = juce::jmax(76, juce::jmin(112, h - widthY - 176));
    const auto widthMargin = 52;
    const auto widthGap = 12;
    const auto width = juce::jmax(100, (w - 2 * widthMargin - 3 * widthGap) / 4);
    for (int i = 0; i < 4; ++i)
    {
        const auto x = widthMargin + i * (width + widthGap);
        widthLabels[static_cast<size_t>(i)].setBounds(x, widthY - 20, width, 18);
        widthSliders[static_cast<size_t>(i)].setBounds(x, widthY, width, widthHeight);
        stereoPresetSelectors[static_cast<size_t>(i)].setBounds(x, widthY + widthHeight + 8,
                                                                  width, 28);
    }

    const auto compMargin = 40;
    const auto compGap = 10;
    const auto compWidth = juce::jmax(100, (w - 2 * compMargin - 5 * compGap) / 6);
    const auto compHeight = juce::jmax(126, juce::jmin(176, h - 390));
    juce::Slider* compSliders[] {
        &compAttackSlider, &compReleaseSlider, &compRatioSlider,
        &compInputSlider, &compOutputSlider, &compThresholdSlider
    };
    for (int i = 0; i < 6; ++i)
    {
        const auto x = compMargin + i * (compWidth + compGap);
        compressorControlLabels[static_cast<size_t>(i)].setBounds(x, 238, compWidth, 18);
        compSliders[i]->setBounds(x, 256, compWidth, compHeight);
    }
}

void NorthstarMasteringAudioProcessorEditor::timerCallback()
{
    SpectrumArray bins {};
    processor.copySpectrum(bins);
    spectrum.setValues(bins);
    eqGraph.setSpectrum(bins);
    meter.setLevels(processor.getPeakDb(), processor.getOutputLoudness(),
                    processor.getGainReduction());
    updateLabels();
    updateModuleStates();
    if (page == Page::eq)
        updateEQControls();
}

void NorthstarMasteringAudioProcessorEditor::Meter::setLevels(
    float peak, float loudness, float gainReduction)
{
    peakLevel = normalizedMeter(peak);
    loudnessLevel = normalizedMeter(loudness);
    reductionLevel = juce::jlimit(0.0f, 1.0f, gainReduction / 12.0f);
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::Meter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(border);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(bounds.withWidth(bounds.getWidth() * loudnessLevel).reduced(1.0f), 3.0f);
    g.setColour(warm);
    g.fillRect(bounds.getX() + bounds.getWidth() * peakLevel - 2.0f, bounds.getY(),
               2.0f, bounds.getHeight());
    g.setColour(danger.withAlpha(reductionLevel));
    g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth() * reductionLevel, bounds.getHeight());
}

void NorthstarMasteringAudioProcessorEditor::Spectrum::setValues(const SpectrumArray& values)
{
    bins = values;
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::Spectrum::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(surfaceRaised);
    g.fillRoundedRectangle(bounds, 4.0f);
    juce::Path path;
    const auto step = bounds.getWidth() / static_cast<float>(bins.size() - 1);
    for (size_t index = 0; index < bins.size(); ++index)
    {
        const auto x = bounds.getX() + step * static_cast<float>(index);
        const auto level = juce::jlimit(0.0f, 1.0f, (bins[index] - spectrumFloorDb) / -spectrumFloorDb);
        const auto y = bounds.getBottom() - level * bounds.getHeight();
        if (index == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }
    g.setColour(accent);
    g.strokePath(path, juce::PathStrokeType(1.3f));
}

// ---------------------------------------------------------------------------
// EQ graph
// ---------------------------------------------------------------------------

NorthstarMasteringAudioProcessorEditor::EQGraph::EQGraph(
    NorthstarMasteringAudioProcessor& processorToUse)
    : processor(processorToUse)
{
    spectrumBins.fill(-120.0f);
    peakHold.fill(-120.0f);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::setSpectrum(const SpectrumArray& values)
{
    spectrumBins = values;
    for (size_t i = 0; i < peakHold.size(); ++i)
        peakHold[i] = juce::jmax(values[i], peakHold[i] - 0.4f);
    repaint();
}

juce::Rectangle<float> NorthstarMasteringAudioProcessorEditor::EQGraph::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft(40.0f).withTrimmedRight(40.0f)
        .withTrimmedTop(46.0f).withTrimmedBottom(22.0f);
}

float NorthstarMasteringAudioProcessorEditor::EQGraph::xForFrequency(float frequency) const
{
    const auto plot = plotArea();
    const auto normalized = std::log10(juce::jlimit(20.0f, 20000.0f, frequency) / 20.0f)
        / std::log10(1000.0f);
    return plot.getX() + normalized * plot.getWidth();
}

float NorthstarMasteringAudioProcessorEditor::EQGraph::yForGain(float gain) const
{
    const auto plot = plotArea();
    return plot.getCentreY() - gain / displayRangeDb * plot.getHeight() * 0.5f;
}

float NorthstarMasteringAudioProcessorEditor::EQGraph::yForSpectrum(float db) const
{
    const auto plot = plotArea();
    const auto normalized = juce::jlimit(0.0f, 1.0f,
        (db - spectrumBottomDb) / (spectrumTopDb - spectrumBottomDb));
    return plot.getBottom() - normalized * plot.getHeight();
}

float NorthstarMasteringAudioProcessorEditor::EQGraph::frequencyForX(float x) const
{
    const auto plot = plotArea();
    const auto normalized = juce::jlimit(0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
    return 20.0f * std::pow(1000.0f, normalized);
}

float NorthstarMasteringAudioProcessorEditor::EQGraph::gainForY(float y) const
{
    const auto plot = plotArea();
    return juce::jlimit(-18.0f, 18.0f,
                        (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * displayRangeDb);
}

int NorthstarMasteringAudioProcessorEditor::EQGraph::bandAtPosition(
    juce::Point<float> position) const
{
    int nearest = -1;
    auto distance = std::numeric_limits<float>::max();
    for (int band = 0; band < NorthstarMasteringAudioProcessor::eqBandCount; ++band)
    {
        const juce::Point<float> point(xForFrequency(processor.getEQFrequency(band)),
                                       yForGain(processor.getEQGain(band)));
        const auto nextDistance = point.getDistanceFrom(position);
        if (nextDistance < distance)
        {
            nearest = band;
            distance = nextDistance;
        }
    }
    return distance < hitRadius ? nearest : -1;
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::setSelectedBand(int band)
{
    selectedBand = juce::jlimit(0, NorthstarMasteringAudioProcessor::eqBandCount - 1, band);
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::selectBand(int band)
{
    selectedBand = band;
    if (onBandSelected != nullptr)
        onBandSelected(selectedBand);
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseDown(const juce::MouseEvent& event)
{
    mousePosition = event.position;
    const auto hit = bandAtPosition(event.position);
    if (hit >= 0 && event.mods.isLeftButtonDown())
    {
        dragging = true;
        dragStartMouse = event.position;
        dragStartFrequencyX = xForFrequency(processor.getEQFrequency(hit));
        dragStartGainY = yForGain(processor.getEQGain(hit));
        selectBand(hit);
    }
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseDrag(const juce::MouseEvent& event)
{
    mousePosition = event.position;
    if (!dragging)
        return;
    // The node keeps its offset from the cursor; Shift slows the movement down 5x.
    const auto scale = event.mods.isShiftDown() ? 0.2f : 1.0f;
    const auto delta = (event.position - dragStartMouse) * scale;
    processor.setEQFrequency(selectedBand, frequencyForX(dragStartFrequencyX + delta.x));
    processor.setEQGain(selectedBand, gainForY(dragStartGainY + delta.y));
    if (onBandSelected != nullptr)
        onBandSelected(selectedBand);
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseUp(const juce::MouseEvent&)
{
    dragging = false;
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseMove(const juce::MouseEvent& event)
{
    hasMouse = true;
    mousePosition = event.position;
    hoverBand = bandAtPosition(event.position);
    setMouseCursor(hoverBand >= 0 ? juce::MouseCursor::PointingHandCursor
                                  : juce::MouseCursor::CrosshairCursor);
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseExit(const juce::MouseEvent&)
{
    hasMouse = false;
    hoverBand = -1;
    repaint();
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseDoubleClick(const juce::MouseEvent& event)
{
    const auto hit = bandAtPosition(event.position);
    if (hit < 0)
        return;
    processor.setEQGain(hit, 0.0f);
    selectBand(hit);
}

// Wheel over a node changes its bandwidth: up = wider (lower Q), down = narrower (higher Q).
void NorthstarMasteringAudioProcessorEditor::EQGraph::mouseWheelMove(
    const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto hit = bandAtPosition(event.position);
    if (hit < 0)
    {
        Component::mouseWheelMove(event, wheel);
        return;
    }

    auto delta = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    if (delta == 0.0f)
        return;
    const auto sensitivity = event.mods.isShiftDown() ? 0.15f : 0.55f;
    processor.setEQQ(hit, processor.getEQQ(hit) * std::exp(-delta * sensitivity));
    selectBand(hit);
}

void NorthstarMasteringAudioProcessorEditor::EQGraph::paint(juce::Graphics& g)
{
    using Processor = NorthstarMasteringAudioProcessor;
    const auto bounds = getLocalBounds().toFloat();
    const auto plot = plotArea();
    const auto sampleRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
    const auto smallFont = juce::Font(juce::FontOptions(9.0f));
    const auto infoFont = juce::Font(juce::FontOptions(10.5f, juce::Font::bold));

    g.setColour(juce::Colour(0xff0d151c));
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(juce::Colour(0xff0a1218));
    g.fillRect(plot);

    // ---- Frequency grid: 20 Hz ... 20 kHz, every 1..9 x 10^n, labels without overlap ----
    g.setFont(smallFont);
    float lastLabelRight = -1000.0f;
    for (const float base : { 10.0f, 100.0f, 1000.0f, 10000.0f })
    {
        for (int multiple = 1; multiple <= 9; ++multiple)
        {
            const auto frequency = base * static_cast<float>(multiple);
            if (frequency < 19.9f || frequency > 20001.0f)
                continue;
            const auto x = xForFrequency(frequency);
            g.setColour(border.withAlpha(multiple == 1 ? 0.95f : 0.42f));
            g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());

            const bool labelled = multiple == 1 || multiple == 2 || multiple == 3 || multiple == 4
                                  || multiple == 5 || multiple == 6 || multiple == 8;
            if (!labelled)
                continue;
            const auto label = frequency >= 1000.0f
                ? juce::String(static_cast<int>(frequency / 1000.0f)) + "k"
                : juce::String(static_cast<int>(frequency));
            const auto labelWidth = juce::Font(juce::FontOptions(9.0f)).getStringWidthFloat(label) + 4.0f;
            const auto left = x - labelWidth * 0.5f;
            if (left < lastLabelRight + 3.0f)
                continue;
            g.setColour(muted);
            g.drawText(label, juce::Rectangle<float>(left, plot.getBottom() + 3.0f, labelWidth, 14.0f),
                       juce::Justification::centred);
            lastLabelRight = left + labelWidth;
        }
    }
    g.setColour(muted);
    g.drawText("Hz", juce::Rectangle<float>(plot.getRight() + 4.0f, plot.getBottom() + 3.0f, 30.0f, 14.0f),
               juce::Justification::left);

    // ---- Gain grid: every 3 dB, +/- 18 dB is the parameter limit ----
    for (int db = -21; db <= 21; db += 3)
    {
        const auto y = yForGain(static_cast<float>(db));
        if (db == 0)
            g.setColour(accent.withAlpha(0.5f));
        else if (std::abs(db) == 18)
            g.setColour(warm.withAlpha(0.35f));
        else
            g.setColour(border.withAlpha(0.55f));
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        if (std::abs(db) <= 18)
        {
            g.setColour(db == 0 ? accent : muted);
            g.drawText(formatDb(static_cast<float>(db), 0),
                       juce::Rectangle<float>(0.0f, y - 7.0f, 36.0f, 14.0f),
                       juce::Justification::centredRight);
        }
    }
    g.setColour(muted);
    g.drawText("dB", juce::Rectangle<float>(4.0f, plot.getY() - 14.0f, 30.0f, 12.0f),
               juce::Justification::centredLeft);

    // ---- Input spectrum scale (right side) ----
    g.setColour(juce::Colour(0xff6bb1c2).withAlpha(0.85f));
    for (int db = -10; db >= -80; db -= 10)
    {
        const auto y = yForSpectrum(static_cast<float>(db));
        g.drawHorizontalLine(juce::roundToInt(y), plot.getRight(), plot.getRight() + 4.0f);
        g.drawText(juce::String(db), juce::Rectangle<float>(plot.getRight() + 5.0f, y - 7.0f, 32.0f, 14.0f),
                   juce::Justification::centredLeft);
    }
    g.drawText("dBFS", juce::Rectangle<float>(plot.getRight() + 4.0f, plot.getY() - 14.0f, 36.0f, 12.0f),
               juce::Justification::centredLeft);

    const auto focusBand = dragging ? selectedBand : (hoverBand >= 0 ? hoverBand : selectedBand);
    const auto cursorInside = hasMouse && plot.contains(mousePosition);
    float cursorEqDb = 0.0f;

    {   // Everything in this block is clipped to the plot rectangle.
    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(plot.toNearestInt());

    // ---- Spectrum + peak hold ----
    {
        juce::Path shape, line, peak;
        shape.startNewSubPath(plot.getX(), plot.getBottom());
        const auto count = static_cast<int>(spectrumBins.size());
        for (int i = 0; i < count; ++i)
        {
            const auto x = plot.getX() + (static_cast<float>(i) + 0.5f) / static_cast<float>(count)
                                             * plot.getWidth();
            const auto y = yForSpectrum(spectrumBins[static_cast<size_t>(i)]);
            const auto yPeak = yForSpectrum(peakHold[static_cast<size_t>(i)]);
            shape.lineTo(x, y);
            if (i == 0) { line.startNewSubPath(x, y); peak.startNewSubPath(x, yPeak); }
            else { line.lineTo(x, y); peak.lineTo(x, yPeak); }
        }
        shape.lineTo(plot.getRight(), plot.getBottom());
        shape.closeSubPath();
        g.setColour(juce::Colour(0xff3b7b8c).withAlpha(0.20f));
        g.fillPath(shape);
        g.setColour(juce::Colour(0xff6bb1c2).withAlpha(0.28f));
        g.strokePath(peak, juce::PathStrokeType(0.8f));
        g.setColour(juce::Colour(0xff6bb1c2).withAlpha(0.75f));
        g.strokePath(line, juce::PathStrokeType(1.1f));
    }

    // ---- Bandwidth of the hovered / selected node ----
    {
        const auto centre = processor.getEQFrequency(focusBand);
        const auto k = 1.0f / (2.0f * juce::jmax(0.1f, processor.getEQQ(focusBand)));
        const auto root = std::sqrt(1.0f + k * k);
        const auto x1 = xForFrequency(centre * (root - k));
        const auto x2 = xForFrequency(centre * (root + k));
        g.setColour(accent.withAlpha(0.07f));
        g.fillRect(juce::Rectangle<float>(x1, plot.getY(), x2 - x1, plot.getHeight()));
        const float dashes[] { 4.0f, 4.0f };
        g.setColour(accent.withAlpha(0.55f));
        g.drawDashedLine(juce::Line<float>(x1, plot.getY(), x1, plot.getBottom()), dashes, 2, 1.0f);
        g.drawDashedLine(juce::Line<float>(x2, plot.getY(), x2, plot.getBottom()), dashes, 2, 1.0f);
    }

    // ---- Exact EQ response (same math as the FIR design), one sample per pixel ----
    const auto points = juce::jmax(64, static_cast<int>(plot.getWidth()));
    std::vector<float> xs(static_cast<size_t>(points));
    std::vector<double> phis(static_cast<size_t>(points));
    std::vector<float> totalDb(static_cast<size_t>(points), 0.0f);
    for (int k = 0; k < points; ++k)
    {
        const auto x = plot.getX() + plot.getWidth() * static_cast<float>(k) / static_cast<float>(points - 1);
        xs[static_cast<size_t>(k)] = x;
        const auto s = std::sin(juce::MathConstants<double>::pi * frequencyForX(x) / sampleRate);
        phis[static_cast<size_t>(k)] = s * s;
    }
    const auto zeroY = yForGain(0.0f);
    const auto clampedY = [this](float db)
    {
        return yForGain(juce::jlimit(-displayRangeDb, displayRangeDb, db));
    };

    for (int band = 0; band < Processor::eqBandCount; ++band)
    {
        const auto gain = processor.getEQEffectiveGain(band);
        if (std::abs(gain) < 0.02f)
            continue;
        const auto coefficients = Processor::makePeakCoefficients(
            sampleRate,
            juce::jlimit(20.0, sampleRate * 0.45, static_cast<double>(processor.getEQFrequency(band))),
            gain, processor.getEQQ(band));
        juce::Path bandShape;
        bandShape.startNewSubPath(xs.front(), zeroY);
        for (int k = 0; k < points; ++k)
        {
            const auto db = static_cast<float>(
                10.0 * std::log10(Processor::biquadPowerRatioFromPhi(coefficients, phis[static_cast<size_t>(k)])));
            totalDb[static_cast<size_t>(k)] += db;
            bandShape.lineTo(xs[static_cast<size_t>(k)], clampedY(db));
        }
        bandShape.lineTo(xs.back(), zeroY);
        bandShape.closeSubPath();
        const auto focused = band == selectedBand || band == focusBand;
        g.setColour((processor.getEQDynamic(band) ? warm : accent).withAlpha(focused ? 0.16f : 0.05f));
        g.fillPath(bandShape);
    }

    {
        juce::Path curve, fill;
        fill.startNewSubPath(xs.front(), zeroY);
        for (int k = 0; k < points; ++k)
        {
            const auto x = xs[static_cast<size_t>(k)];
            const auto y = clampedY(totalDb[static_cast<size_t>(k)]);
            fill.lineTo(x, y);
            if (k == 0) curve.startNewSubPath(x, y);
            else curve.lineTo(x, y);
        }
        fill.lineTo(xs.back(), zeroY);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(0.13f));
        g.fillPath(fill);
        g.setColour(accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // ---- Crosshair ----
    if (cursorInside)
    {
        const auto index = juce::jlimit(0, points - 1, juce::roundToInt(
            (mousePosition.x - plot.getX()) / plot.getWidth() * static_cast<float>(points - 1)));
        cursorEqDb = totalDb[static_cast<size_t>(index)];
        g.setColour(text.withAlpha(0.22f));
        g.drawVerticalLine(juce::roundToInt(mousePosition.x), plot.getY(), plot.getBottom());
        g.drawHorizontalLine(juce::roundToInt(mousePosition.y), plot.getX(), plot.getRight());
    }

    // ---- Nodes (numbered) ----
    for (int band = 0; band < Processor::eqBandCount; ++band)
    {
        const auto x = xForFrequency(processor.getEQFrequency(band));
        const auto y = yForGain(processor.getEQGain(band));
        const auto selected = band == selectedBand;
        const auto hovered = band == hoverBand;
        const auto radius = (selected || hovered) ? 9.0f : 7.0f;
        const auto dynamic = processor.getEQDynamic(band);
        const auto idle = std::abs(processor.getEQGain(band)) < 0.02f;
        g.setColour((selected ? text : (dynamic ? warm : accent)).withAlpha(idle && !selected ? 0.6f : 1.0f));
        g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
        if (hovered || selected)
        {
            g.setColour(accent.withAlpha(0.55f));
            g.drawEllipse(x - radius - 3.0f, y - radius - 3.0f, (radius + 3.0f) * 2.0f,
                          (radius + 3.0f) * 2.0f, 1.0f);
        }
        if (dynamic)
        {
            g.setColour(warm);
            g.drawEllipse(x - radius - 2.0f, y - radius - 2.0f, (radius + 2.0f) * 2.0f,
                          (radius + 2.0f) * 2.0f, 1.0f);
        }
        g.setColour(background);
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText(juce::String(band + 1), juce::Rectangle<float>(x - radius, y - radius, radius * 2.0f, radius * 2.0f),
                   juce::Justification::centred);
    }

    }   // end of clipped block

    // ---- Read-outs above the plot (not clipped) ----
    const auto infoLeft = plot.getX();
    {
        const auto centre = processor.getEQFrequency(focusBand);
        const auto gain = processor.getEQGain(focusBand);
        const auto q = processor.getEQQ(focusBand);
        const auto k = 1.0f / (2.0f * juce::jmax(0.1f, q));
        const auto root = std::sqrt(1.0f + k * k);
        const auto bandwidthOctaves = 2.0f * std::asinh(k) / std::log(2.0f);
        juce::String info = "BAND " + juce::String(focusBand + 1);
        if (processor.getEQDynamic(focusBand))
            info << " DYN (now " << formatDb(processor.getEQEffectiveGain(focusBand)) << " dB)";
        info << "   " << formatFrequency(centre) << "  " << noteName(centre)
             << "   " << formatDb(gain) << " dB   Q " << juce::String(q, 2)
             << "   BW " << juce::String(bandwidthOctaves, 2) << " oct   "
             << formatFrequency(centre * (root - k)) << " - " << formatFrequency(centre * (root + k));
        g.setColour(focusBand == selectedBand ? text : accent);
        g.setFont(infoFont);
        g.drawText(info, juce::Rectangle<float>(infoLeft, 6.0f, bounds.getRight() - infoLeft - 8.0f, 16.0f),
                   juce::Justification::centredLeft, true);
    }
    {
        juce::String cursorText = "CURSOR  --";
        if (cursorInside)
        {
            const auto frequency = frequencyForX(mousePosition.x);
            const auto binIndex = juce::jlimit(0, static_cast<int>(spectrumBins.size()) - 1,
                static_cast<int>((mousePosition.x - plot.getX()) / plot.getWidth()
                                 * static_cast<float>(spectrumBins.size())));
            cursorText = "CURSOR  " + formatFrequency(frequency) + "  " + noteName(frequency)
                         + "   EQ " + formatDb(cursorEqDb) + " dB   INPUT "
                         + juce::String(spectrumBins[static_cast<size_t>(binIndex)], 1) + " dBFS";
        }
        g.setColour(muted);
        g.setFont(smallFont);
        g.drawText(cursorText, juce::Rectangle<float>(infoLeft, 24.0f, plot.getWidth() * 0.55f, 16.0f),
                   juce::Justification::centredLeft, true);

        const auto latencyMs = static_cast<float>(processor.getLatencySamples() * 1000.0 / sampleRate);
        g.setColour(accent);
        g.drawText("LINEAR PHASE  |  PHASE SHIFT 0.0 DEG  |  DELAY " + juce::String(latencyMs, 1) + " ms",
                   juce::Rectangle<float>(plot.getRight() - plot.getWidth() * 0.45f, 24.0f,
                                          plot.getWidth() * 0.45f, 16.0f),
                   juce::Justification::centredRight, true);
    }
}
