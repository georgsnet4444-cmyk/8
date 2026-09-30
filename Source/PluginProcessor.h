#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

class NorthstarMasteringAudioProcessor final : public juce::AudioProcessor,
                                               private juce::Timer
{
public:
    static constexpr int eqBandCount = 15;
    static constexpr int stereoBandCount = 4;

    // Live spectrum: 256 log-spaced bins between 20 Hz and 20 kHz, values in dBFS.
    static constexpr int spectrumBinCount = 256;

    // Linear-phase EQ: symmetric FIR built from a zero-phase magnitude response.
    // Group delay is constant (eqTaps - 1) / 2 samples, phase is exactly linear.
    static constexpr int eqFftOrder = 14;
    static constexpr int eqFftSize = 1 << eqFftOrder;
    static constexpr int eqTaps = 8191;

    struct BiquadCoefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    };

    // RBJ peaking filter, used only to describe the magnitude of each EQ node
    // (for the FIR design and for the graph). No audio runs through it.
    static BiquadCoefficients makePeakCoefficients(double sampleRate, double frequency,
                                                   double gainDb, double q) noexcept;
    static double biquadPowerRatioFromPhi(const BiquadCoefficients& c, double phi) noexcept;
    static double biquadPowerRatio(const BiquadCoefficients& c, double sampleRate,
                                   double frequency) noexcept;
    static float effectiveGain(float gainDb, bool dynamic, float envelope) noexcept;

    NorthstarMasteringAudioProcessor();
    ~NorthstarMasteringAudioProcessor() override;

    using juce::AudioProcessor::processBlock;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void requestAnalysis();
    bool isAnalysisRunning() const noexcept { return analysisRunning.load(); }
    float getAnalysisProgress() const noexcept { return analysisProgress.load(); }
    float getLoudnessEstimate() const noexcept { return loudnessEstimate.load(); }
    float getPeakDb() const noexcept { return peakDb.load(); }
    float getOutputLoudness() const noexcept { return outputLoudness.load(); }
    float getGainReduction() const noexcept { return gainReductionDb.load(); }
    bool hasAnalysis() const noexcept { return analysisComplete.load(); }
    void copySpectrum(std::array<float, spectrumBinCount>& destination) const noexcept;

    float getEQFrequency(int band) const noexcept;
    float getEQGain(int band) const noexcept;
    float getEQQ(int band) const noexcept;
    bool getEQDynamic(int band) const noexcept;
    // Gain that is really applied right now (differs from getEQGain for dynamic nodes).
    float getEQEffectiveGain(int band) const noexcept;
    void setEQFrequency(int band, float value);
    void setEQGain(int band, float value);
    void setEQQ(int band, float value);
    void setEQDynamic(int band, bool enabled);
    void resetEQToAuto();

    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
        double process(double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
        void setBandpass(double sampleRate, double frequency, double q) noexcept;
    };

    struct ParamPointers
    {
        std::atomic<float>* eqMode = nullptr;
        std::atomic<float>* eqEnabled = nullptr;
        std::atomic<float>* eqDynamic = nullptr;
        std::array<std::atomic<float>*, eqBandCount> eqFreq {};
        std::array<std::atomic<float>*, eqBandCount> eqGain {};
        std::array<std::atomic<float>*, eqBandCount> eqQ {};
        std::array<std::atomic<float>*, eqBandCount> eqDyn {};
        std::atomic<float>* targetLufs = nullptr;
        std::atomic<float>* volume = nullptr;
        std::atomic<float>* loudnessEnabled = nullptr;
        std::atomic<float>* saturationMix = nullptr;
        std::atomic<float>* saturationPreset = nullptr;
        std::atomic<float>* saturationEnabled = nullptr;
        std::atomic<float>* stereoEnabled = nullptr;
        std::array<std::atomic<float>*, stereoBandCount - 1> xover {};
        std::array<std::atomic<float>*, stereoBandCount> width {};
        std::array<std::atomic<float>*, stereoBandCount> preset {};
        std::atomic<float>* compEnabled = nullptr;
        std::atomic<float>* compAttack = nullptr;
        std::atomic<float>* compRelease = nullptr;
        std::atomic<float>* compRatio = nullptr;
        std::atomic<float>* compInput = nullptr;
        std::atomic<float>* compOutput = nullptr;
        std::atomic<float>* compThreshold = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    using Filter = juce::dsp::IIR::Filter<float>;
    using Coefficients = juce::dsp::IIR::Coefficients<float>;

    static constexpr int spectrumFftOrder = 12;
    static constexpr int spectrumSize = 1 << spectrumFftOrder;
    static constexpr int spectrumHop = spectrumSize / 2;
    static constexpr int analysisDurationSeconds = 10;

    void timerCallback() override;
    void rebuildEqImpulse(bool force);
    void finishAnalysis();
    void updateCrossovers();
    void pushSpectrumSample(float sample) noexcept;
    void computeSpectrum() noexcept;
    void applyStereoImage(float& left, float& right, const std::array<float, stereoBandCount>& widths,
                          const std::array<int, stereoBandCount>& presets) noexcept;
    float applySaturation(float sample, float mix, int preset) const noexcept;
    static float toDb(float value) noexcept;
    static juce::String eqId(const char* prefix, int band);
    static juce::String stereoId(const char* prefix, int band);

    ParamPointers p;

    std::atomic<double> currentSampleRate { 0.0 };

    // --- Linear-phase EQ ---
    juce::dsp::Convolution eqConvolution { juce::dsp::Convolution::Latency { 512 } };
    juce::dsp::FFT eqFft { eqFftOrder };
    std::vector<float> eqWork;
    juce::CriticalSection eqBuildLock;
    std::array<float, eqBandCount * 3 + 2> lastEqSignature {};
    bool eqSignatureValid = false;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 16384 };
    juce::AudioBuffer<float> dryBuffer;

    // --- Dynamic EQ side-chain detectors (never in the audio path) ---
    std::array<Biquad, eqBandCount> dynamicDetectors;
    std::array<float, eqBandCount> dynamicLevels {};
    std::array<std::atomic<float>, eqBandCount> dynamicEnvelopes {};

    // --- Loudness metering / stereo ---
    std::array<std::array<Filter, 2>, 2> loudnessFilters;
    std::array<float, stereoBandCount - 1> crossoverFrequencies { 120.0f, 1000.0f, 6000.0f };
    std::array<float, stereoBandCount - 1> crossoverCoefficients { 0.9f, 0.9f, 0.9f };
    std::array<std::array<float, stereoBandCount>, 2> stereoLowStates {};

    // --- Spectrum analyser ---
    std::array<std::atomic<float>, spectrumBinCount> spectrumBins {};
    juce::dsp::FFT spectrumFft { spectrumFftOrder };
    juce::dsp::WindowingFunction<float> spectrumWindow {
        spectrumSize, juce::dsp::WindowingFunction<float>::hann, false
    };
    std::array<float, spectrumSize> spectrumRing {};
    std::array<float, spectrumSize * 2> spectrumWork {};
    std::array<float, spectrumBinCount> spectrumSmooth {};
    int spectrumWritePosition = 0;
    int spectrumSinceHop = 0;

    float compressorEnvelope = 0.0f;
    float smoothPeak = -60.0f;
    float smoothInputLoudness = -60.0f;
    float smoothOutputLoudness = -60.0f;

    std::atomic<bool> analysisRequested { false };
    std::atomic<bool> analysisRunning { false };
    std::atomic<bool> analysisComplete { false };
    std::atomic<float> analysisProgress { 0.0f };
    std::atomic<float> loudnessEstimate { -60.0f };
    std::atomic<float> learnedLufs { -60.0f };
    std::atomic<float> outputLoudness { -60.0f };
    std::atomic<float> peakDb { -60.0f };
    std::atomic<float> gainReductionDb { 0.0f };

    bool collectingAnalysis = false;
    int64_t analyzedSamples = 0;
    double analysisEnergy = 0.0;
    double analysisPeak = 0.0;
    std::array<double, 3> analysisBandEnergy {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NorthstarMasteringAudioProcessor)
};
