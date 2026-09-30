#pragma once

#include "PluginProcessor.h"

#include <array>
#include <functional>

class NorthstarMasteringAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                     private juce::Timer
{
public:
    explicit NorthstarMasteringAudioProcessorEditor(NorthstarMasteringAudioProcessor&);
    ~NorthstarMasteringAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    enum class Page { eq, loudness, saturation, stereo, compressor };

    using SpectrumArray = std::array<float, NorthstarMasteringAudioProcessor::spectrumBinCount>;

    class Meter final : public juce::Component
    {
    public:
        void setLevels(float peak, float loudness, float gainReduction);
        void paint(juce::Graphics&) override;
    private:
        float peakLevel = 0.0f;
        float loudnessLevel = 0.0f;
        float reductionLevel = 0.0f;
    };

    class Spectrum final : public juce::Component
    {
    public:
        void setValues(const SpectrumArray& values);
        void paint(juce::Graphics&) override;
    private:
        SpectrumArray bins {};
    };

    class EQGraph final : public juce::Component
    {
    public:
        explicit EQGraph(NorthstarMasteringAudioProcessor& processorToUse);
        void setSelectedBand(int band);
        void setSpectrum(const SpectrumArray& values);
        int getSelectedBand() const noexcept { return selectedBand; }
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        std::function<void(int)> onBandSelected;

    private:
        static constexpr float displayRangeDb = 21.0f;   // vertical EQ range: +/- 21 dB
        static constexpr float spectrumTopDb = 0.0f;
        static constexpr float spectrumBottomDb = -90.0f;
        static constexpr float hitRadius = 22.0f;

        juce::Rectangle<float> plotArea() const;
        int bandAtPosition(juce::Point<float> position) const;
        void selectBand(int band);
        float xForFrequency(float frequency) const;
        float yForGain(float gain) const;
        float yForSpectrum(float db) const;
        float frequencyForX(float x) const;
        float gainForY(float y) const;

        NorthstarMasteringAudioProcessor& processor;
        int selectedBand = 0;
        int hoverBand = -1;
        bool dragging = false;
        bool hasMouse = false;
        juce::Point<float> mousePosition;
        juce::Point<float> dragStartMouse;
        float dragStartFrequencyX = 0.0f;
        float dragStartGainY = 0.0f;
        SpectrumArray spectrumBins {};
        SpectrumArray peakHold {};
    };

    void timerCallback() override;
    void styleSlider(juce::Slider&, const juce::String& suffix);
    void styleControlLabel(juce::Label&, const juce::String& text);
    void styleButton(juce::TextButton&, bool toggle = true);
    void stylePowerButton(juce::TextButton&);
    void styleToggle(juce::ToggleButton&);
    void setPage(Page);
    void updateEQControls();
    void updateLabels();
    void updateModuleStates();

    NorthstarMasteringAudioProcessor& processor;
    Page page = Page::eq;
    int selectedEQBand = 0;
    bool updatingEQControls = false;

    juce::Label brandLabel;
    juce::Label subtitleLabel;
    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::Label reductionLabel;
    juce::Label statusLabel;

    juce::TextButton eqTab { "EQUALIZATION" };
    juce::TextButton loudnessTab { "LOUDNESS" };
    juce::TextButton saturationTab { "SATURATION" };
    juce::TextButton stereoTab { "STEREO IMAGER" };
    juce::TextButton compressorTab { "OPTO COMPRESSOR" };
    juce::TextButton bypassButton { "BYPASS" };
    juce::TextButton analyzeButton { "ANALYZE 10 SEC" };

    // One full-disable button per tab.
    juce::TextButton eqPower { "POWER  ON" };
    juce::TextButton loudnessPower { "POWER  ON" };
    juce::TextButton saturationPower { "POWER  ON" };
    juce::TextButton stereoPower { "POWER  ON" };
    juce::TextButton compressorPower { "POWER  ON" };

    juce::ComboBox eqModeSelector;
    juce::ToggleButton eqDynamicButton { "DYNAMIC" };
    juce::TextButton eqAutoButton { "AUTO RESET" };
    juce::Slider eqFrequencySlider;
    juce::Slider eqGainSlider;
    juce::Slider eqQSlider;
    juce::Label eqBandLabel;
    juce::Label eqPhaseLabel;
    std::array<juce::Label, 3> eqControlLabels;

    juce::Slider targetSlider;
    juce::Slider volumeSlider;
    juce::TextButton learnButton { "LEARN 10 SEC" };
    std::array<juce::Label, 2> loudnessControlLabels;

    juce::Slider saturationMixSlider;
    juce::ComboBox saturationPresetSelector;
    juce::Label saturationControlLabel;

    std::array<juce::Slider, 3> crossoverSliders;
    std::array<juce::Slider, 4> widthSliders;
    std::array<juce::ComboBox, 4> stereoPresetSelectors;
    std::array<juce::Label, 3> crossoverLabels;
    std::array<juce::Label, 4> widthLabels;

    juce::Slider compAttackSlider;
    juce::Slider compReleaseSlider;
    juce::Slider compRatioSlider;
    juce::Slider compInputSlider;
    juce::Slider compOutputSlider;
    juce::Slider compThresholdSlider;
    std::array<juce::Label, 6> compressorControlLabels;

    Meter meter;
    Spectrum spectrum;
    EQGraph eqGraph;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SliderAttachment> targetAttachment;
    std::unique_ptr<SliderAttachment> volumeAttachment;
    std::unique_ptr<SliderAttachment> saturationMixAttachment;
    std::unique_ptr<ComboAttachment> saturationPresetAttachment;
    std::array<std::unique_ptr<SliderAttachment>, 3> crossoverAttachments;
    std::array<std::unique_ptr<SliderAttachment>, 4> widthAttachments;
    std::array<std::unique_ptr<ComboAttachment>, 4> stereoPresetAttachments;
    std::unique_ptr<SliderAttachment> compAttackAttachment;
    std::unique_ptr<SliderAttachment> compReleaseAttachment;
    std::unique_ptr<SliderAttachment> compRatioAttachment;
    std::unique_ptr<SliderAttachment> compInputAttachment;
    std::unique_ptr<SliderAttachment> compOutputAttachment;
    std::unique_ptr<SliderAttachment> compThresholdAttachment;
    std::unique_ptr<ComboAttachment> eqModeAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ButtonAttachment> eqPowerAttachment;
    std::unique_ptr<ButtonAttachment> loudnessPowerAttachment;
    std::unique_ptr<ButtonAttachment> saturationPowerAttachment;
    std::unique_ptr<ButtonAttachment> stereoPowerAttachment;
    std::unique_ptr<ButtonAttachment> compressorPowerAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NorthstarMasteringAudioProcessorEditor)
};
