#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float safeFloor = 1.0e-9f;
constexpr float pi = juce::MathConstants<float>::pi;

juce::AudioParameterFloatAttributes label(const juce::String& value)
{
    return juce::AudioParameterFloatAttributes().withLabel(value);
}

juce::NormalisableRange<float> range(float start, float end, float interval = 0.01f)
{
    return juce::NormalisableRange<float>(start, end, interval);
}
}

// ---------------------------------------------------------------------------
// Static helpers: EQ node magnitude model (shared by the FIR design and the graph)
// ---------------------------------------------------------------------------

NorthstarMasteringAudioProcessor::BiquadCoefficients
NorthstarMasteringAudioProcessor::makePeakCoefficients(double sampleRate, double frequency,
                                                       double gainDb, double q) noexcept
{
    const auto a = std::pow(10.0, gainDb / 40.0);
    const auto w0 = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
    const auto alpha = std::sin(w0) / (2.0 * juce::jmax(0.01, q));
    const auto cosW0 = std::cos(w0);

    BiquadCoefficients c;
    c.b0 = 1.0 + alpha * a;
    c.b1 = -2.0 * cosW0;
    c.b2 = 1.0 - alpha * a;
    c.a0 = 1.0 + alpha / a;
    c.a1 = -2.0 * cosW0;
    c.a2 = 1.0 - alpha / a;
    return c;
}

// |B(e^jw)|^2 written in terms of phi = sin^2(w/2). This form does not suffer from
// cancellation at low frequencies, which matters for narrow bells near 20-60 Hz.
double NorthstarMasteringAudioProcessor::biquadPowerRatioFromPhi(const BiquadCoefficients& c,
                                                                 double phi) noexcept
{
    const auto power = [phi](double x0, double x1, double x2)
    {
        const auto sum = x0 + x1 + x2;
        return sum * sum - 4.0 * (x0 * x1 + 4.0 * x0 * x2 + x1 * x2) * phi
               + 16.0 * x0 * x2 * phi * phi;
    };
    const auto denominator = power(c.a0, c.a1, c.a2);
    if (denominator <= 1.0e-30)
        return 1.0;
    return juce::jmax(0.0, power(c.b0, c.b1, c.b2)) / denominator;
}

double NorthstarMasteringAudioProcessor::biquadPowerRatio(const BiquadCoefficients& c,
                                                          double sampleRate,
                                                          double frequency) noexcept
{
    const auto s = std::sin(juce::MathConstants<double>::pi * frequency / sampleRate);
    return biquadPowerRatioFromPhi(c, s * s);
}

float NorthstarMasteringAudioProcessor::effectiveGain(float gainDb, bool dynamic, float envelope) noexcept
{
    if (!dynamic)
        return gainDb;
    const auto e = juce::jlimit(0.0f, 1.0f, envelope);
    // Dynamic cut: applied only while the band is loud. Dynamic boost: backs off when loud.
    return gainDb < 0.0f ? gainDb * e : gainDb * (1.0f - e);
}

void NorthstarMasteringAudioProcessor::Biquad::setBandpass(double sampleRate, double frequency,
                                                           double q) noexcept
{
    const auto w0 = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
    const auto alpha = std::sin(w0) / (2.0 * q);
    const auto a0 = 1.0 + alpha;
    b0 = alpha / a0;
    b1 = 0.0;
    b2 = -alpha / a0;
    a1 = -2.0 * std::cos(w0) / a0;
    a2 = (1.0 - alpha) / a0;
}

// ---------------------------------------------------------------------------

NorthstarMasteringAudioProcessor::NorthstarMasteringAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "NORTHSTAR_STATE", createParameterLayout())
{
    const auto get = [this](const juce::String& id) { return parameters.getRawParameterValue(id); };
    p.eqMode = get("eqMode");
    p.eqEnabled = get("eqEnabled");
    p.eqDynamic = get("eqDynamic");
    for (int band = 0; band < eqBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        p.eqFreq[i] = get(eqId("eqFreq", band));
        p.eqGain[i] = get(eqId("eqGain", band));
        p.eqQ[i] = get(eqId("eqQ", band));
        p.eqDyn[i] = get(eqId("eqDyn", band));
    }
    p.targetLufs = get("targetLufs");
    p.volume = get("volume");
    p.loudnessEnabled = get("loudnessEnabled");
    p.saturationMix = get("saturationMix");
    p.saturationPreset = get("saturationPreset");
    p.saturationEnabled = get("saturationEnabled");
    p.stereoEnabled = get("stereoEnabled");
    for (int band = 0; band < stereoBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        if (band < stereoBandCount - 1)
            p.xover[i] = get(stereoId("stereoXover", band));
        p.width[i] = get(stereoId("stereoWidth", band));
        p.preset[i] = get(stereoId("stereoPreset", band));
    }
    p.compEnabled = get("compressorEnabled");
    p.compAttack = get("compAttack");
    p.compRelease = get("compRelease");
    p.compRatio = get("compRatio");
    p.compInput = get("compInput");
    p.compOutput = get("compOutput");
    p.compThreshold = get("compThreshold");
    p.bypass = get("bypass");

    eqWork.assign(static_cast<size_t>(eqFftSize) * 2, 0.0f);
    spectrumSmooth.fill(-120.0f);
    for (auto& bin : spectrumBins)
        bin.store(-120.0f);

    // Impulse responses are designed on the message thread, never on the audio thread.
    startTimerHz(30);
}

NorthstarMasteringAudioProcessor::~NorthstarMasteringAudioProcessor()
{
    stopTimer();
}

juce::String NorthstarMasteringAudioProcessor::eqId(const char* prefix, int band)
{
    return juce::String(prefix) + juce::String(band + 1);
}

juce::String NorthstarMasteringAudioProcessor::stereoId(const char* prefix, int band)
{
    return juce::String(prefix) + juce::String(band + 1);
}

juce::AudioProcessorValueTreeState::ParameterLayout
NorthstarMasteringAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<Choice>(
        juce::ParameterID { "eqMode", 1 }, "EQ mode",
        juce::StringArray { "Automatic", "Manual" }, 0));
    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "eqEnabled", 1 }, "Equalizer", true));
    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "eqDynamic", 1 }, "Dynamic EQ", false));

    const std::array<float, eqBandCount> defaults {
        32.0f, 55.0f, 90.0f, 150.0f, 250.0f, 400.0f, 650.0f, 1000.0f,
        1600.0f, 2500.0f, 4000.0f, 6300.0f, 10000.0f, 15000.0f, 19000.0f
    };
    for (int band = 0; band < eqBandCount; ++band)
    {
        layout.add(std::make_unique<Float>(
            juce::ParameterID { eqId("eqFreq", band), 1 }, "EQ frequency " + juce::String(band + 1),
            range(20.0f, 20000.0f, 0.01f), defaults[static_cast<size_t>(band)], label("Hz")));
        layout.add(std::make_unique<Float>(
            juce::ParameterID { eqId("eqGain", band), 1 }, "EQ gain " + juce::String(band + 1),
            range(-18.0f, 18.0f, 0.01f), 0.0f, label("dB")));
        layout.add(std::make_unique<Float>(
            juce::ParameterID { eqId("eqQ", band), 1 }, "EQ Q " + juce::String(band + 1),
            range(0.1f, 12.0f, 0.01f), 0.85f, label("Q")));
        layout.add(std::make_unique<Bool>(
            juce::ParameterID { eqId("eqDyn", band), 1 }, "EQ dynamic " + juce::String(band + 1), false));
    }

    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "loudnessEnabled", 1 }, "Loudness", true));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "targetLufs", 1 }, "Target loudness",
        range(-24.0f, -8.0f, 0.1f), -14.0f, label("LUFS")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "volume", 1 }, "Volume",
        range(-24.0f, 24.0f, 0.01f), 0.0f, label("dB")));

    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "saturationEnabled", 1 }, "Saturation", true));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "saturationMix", 1 }, "Saturation mix",
        range(0.0f, 100.0f, 0.1f), 0.0f, label("%")));
    layout.add(std::make_unique<Choice>(
        juce::ParameterID { "saturationPreset", 1 }, "Saturation preset",
        juce::StringArray { "Warm", "Cold / Airy", "Distortion", "Tube" }, 0));

    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "stereoEnabled", 1 }, "Stereo imager", true));
    const std::array<float, 3> crossoverDefaults { 120.0f, 1000.0f, 6000.0f };
    for (int band = 0; band < stereoBandCount; ++band)
    {
        if (band < stereoBandCount - 1)
            layout.add(std::make_unique<Float>(
                juce::ParameterID { stereoId("stereoXover", band), 1 },
                "Stereo crossover " + juce::String(band + 1),
                range(40.0f, 18000.0f, 0.1f), crossoverDefaults[static_cast<size_t>(band)], label("Hz")));
        layout.add(std::make_unique<Float>(
            juce::ParameterID { stereoId("stereoWidth", band), 1 },
            "Stereo width " + juce::String(band + 1), range(0.0f, 200.0f, 0.1f), 100.0f, label("%")));
        layout.add(std::make_unique<Choice>(
            juce::ParameterID { stereoId("stereoPreset", band), 1 },
            "Stereo preset " + juce::String(band + 1),
            juce::StringArray { "Warm", "Cold / Airy", "Distortion", "Tube" }, 0));
    }

    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "compressorEnabled", 1 }, "OPTO compressor", true));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compAttack", 1 }, "OPTO attack", range(1.0f, 200.0f, 0.1f), 35.0f, label("ms")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compRelease", 1 }, "OPTO release", range(20.0f, 2000.0f, 0.1f), 280.0f, label("ms")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compRatio", 1 }, "OPTO ratio", range(1.0f, 20.0f, 0.01f), 3.0f, label(":1")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compInput", 1 }, "OPTO input", range(-24.0f, 24.0f, 0.01f), 0.0f, label("dB")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compOutput", 1 }, "OPTO output", range(-24.0f, 24.0f, 0.01f), 0.0f, label("dB")));
    layout.add(std::make_unique<Float>(
        juce::ParameterID { "compThreshold", 1 }, "OPTO threshold", range(-48.0f, 0.0f, 0.1f), -18.0f, label("dB")));

    layout.add(std::make_unique<Bool>(
        juce::ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

bool NorthstarMasteringAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return input == output
        && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void NorthstarMasteringAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store(sampleRate);
    compressorEnvelope = 0.0f;
    smoothPeak = smoothInputLoudness = smoothOutputLoudness = -60.0f;
    stereoLowStates = {};
    spectrumRing.fill(0.0f);
    spectrumWork.fill(0.0f);
    spectrumSmooth.fill(-120.0f);
    spectrumWritePosition = 0;
    spectrumSinceHop = 0;
    dynamicLevels.fill(0.0f);
    for (auto& envelope : dynamicEnvelopes)
        envelope.store(0.0f);
    for (auto& detector : dynamicDetectors)
        detector.reset();

    juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock)), 2 };
    for (auto& stage : loudnessFilters)
        for (auto& filter : stage)
        {
            filter.prepare(spec);
            filter.reset();
        }
    for (size_t channel = 0; channel < 2; ++channel)
    {
        loudnessFilters[0][channel].coefficients =
            Coefficients::makeHighPass(sampleRate, juce::jmin(38.0, sampleRate * 0.2), 0.5f);
        loudnessFilters[1][channel].coefficients =
            Coefficients::makeHighShelf(sampleRate, juce::jmin(1682.0, sampleRate * 0.4),
                                        0.707f, juce::Decibels::decibelsToGain(4.0f));
    }

    // Linear-phase EQ: the convolution engine and the FIR are prepared first, then the
    // total delay is reported to the host so the plug-in stays time-aligned.
    eqConvolution.prepare(spec);
    eqConvolution.reset();
    rebuildEqImpulse(true);

    const auto latency = (eqTaps - 1) / 2 + static_cast<int>(eqConvolution.getLatency());
    setLatencySamples(latency);

    // Bypass must have exactly the same delay as the processed path.
    dryDelay.prepare(spec);
    dryDelay.setDelay(static_cast<float>(latency));
    dryBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, false);

    updateCrossovers();
    analysisRunning.store(false);
    analysisComplete.store(false);
    analysisProgress.store(0.0f);
}

void NorthstarMasteringAudioProcessor::releaseResources()
{
}

void NorthstarMasteringAudioProcessor::requestAnalysis()
{
    analysisRequested.store(true);
}

float NorthstarMasteringAudioProcessor::toDb(float value) noexcept
{
    return 20.0f * std::log10(std::max(value, safeFloor));
}

void NorthstarMasteringAudioProcessor::timerCallback()
{
    if (currentSampleRate.load() > 0.0)
        rebuildEqImpulse(false);
}

// Designs the symmetric FIR for the current EQ settings and hands it to the convolution
// engine (which cross-fades between the old and new response). The impulse is built from a
// ZERO-PHASE magnitude, so after the constant delay the EQ adds no phase shift at all.
void NorthstarMasteringAudioProcessor::rebuildEqImpulse(bool force)
{
    const juce::ScopedLock lock(eqBuildLock);
    const auto sampleRate = currentSampleRate.load();
    if (sampleRate <= 0.0)
        return;

    const auto enabled = p.eqEnabled->load() > 0.5f;
    const auto dynamicGlobal = p.eqDynamic->load() > 0.5f;
    const auto maxFrequency = static_cast<float>(sampleRate * 0.45);

    std::array<float, eqBandCount * 3 + 2> signature {};
    for (int band = 0; band < eqBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        const auto dynamic = dynamicGlobal || p.eqDyn[i]->load() > 0.5f;
        const auto gain = enabled ? effectiveGain(p.eqGain[i]->load(), dynamic,
                                                  dynamicEnvelopes[i].load())
                                  : 0.0f;
        signature[i * 3] = juce::jlimit(20.0f, maxFrequency, p.eqFreq[i]->load());
        signature[i * 3 + 1] = std::round(gain * 20.0f) / 20.0f;  // 0.05 dB steps
        signature[i * 3 + 2] = p.eqQ[i]->load();
    }
    signature[eqBandCount * 3] = static_cast<float>(sampleRate);

    if (!force && eqSignatureValid && signature == lastEqSignature)
        return;
    lastEqSignature = signature;
    eqSignatureValid = true;

    std::array<BiquadCoefficients, eqBandCount> active {};
    int activeCount = 0;
    for (int band = 0; band < eqBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        if (std::abs(signature[i * 3 + 1]) > 0.004f)
            active[static_cast<size_t>(activeCount++)] = makePeakCoefficients(
                sampleRate, signature[i * 3], signature[i * 3 + 1], signature[i * 3 + 2]);
    }

    constexpr int half = eqFftSize / 2;
    std::fill(eqWork.begin(), eqWork.end(), 0.0f);
    for (int bin = 0; bin <= half; ++bin)
    {
        const auto s = std::sin(juce::MathConstants<double>::pi * bin / eqFftSize);
        const auto phi = s * s;
        double power = 1.0;
        for (int a = 0; a < activeCount; ++a)
            power *= biquadPowerRatioFromPhi(active[static_cast<size_t>(a)], phi);
        const auto magnitude = static_cast<float>(std::sqrt(power));
        eqWork[static_cast<size_t>(bin) * 2] = magnitude;  // purely real -> zero phase
        if (bin > 0 && bin < half)
            eqWork[static_cast<size_t>(eqFftSize - bin) * 2] = magnitude;
    }
    eqFft.performRealOnlyInverseTransform(eqWork.data());

    juce::AudioBuffer<float> impulse(1, eqTaps);
    auto* destination = impulse.getWritePointer(0);
    constexpr int centre = (eqTaps - 1) / 2;
    for (int k = 0; k < eqTaps; ++k)
    {
        const auto source = static_cast<size_t>((k - centre + eqFftSize) % eqFftSize);
        const auto window = 0.5f - 0.5f * std::cos(2.0f * pi * static_cast<float>(k)
                                                    / static_cast<float>(eqTaps - 1));
        destination[k] = eqWork[source] * window;
    }

    eqConvolution.loadImpulseResponse(std::move(impulse), sampleRate,
                                      juce::dsp::Convolution::Stereo::no,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);
}

void NorthstarMasteringAudioProcessor::updateCrossovers()
{
    const auto maximumFrequency = static_cast<float>(currentSampleRate.load() * 0.45);
    auto previousFrequency = 40.0f;
    for (int index = 0; index < stereoBandCount - 1; ++index)
    {
        const auto i = static_cast<size_t>(index);
        const auto requested = p.xover[i]->load();
        const auto remainingCrossovers = stereoBandCount - 2 - index;
        const auto maximumForBand = maximumFrequency - static_cast<float>(remainingCrossovers) * 20.0f;
        const auto frequency = juce::jlimit(previousFrequency + 20.0f, maximumForBand, requested);
        crossoverFrequencies[i] = frequency;
        crossoverCoefficients[i] = juce::jlimit(0.001f, 0.999f,
            std::exp(-2.0f * pi * frequency / static_cast<float>(currentSampleRate.load())));
        previousFrequency = frequency;
    }
}

float NorthstarMasteringAudioProcessor::applySaturation(float sample, float mix, int preset) const noexcept
{
    if (mix <= 0.0001f)
        return sample;

    const std::array<float, 4> drive { 1.35f, 1.12f, 3.8f, 1.85f };
    const std::array<float, 4> bias { 0.02f, 0.0f, 0.05f, 0.09f };
    const auto index = juce::jlimit(0, 3, preset);
    auto shaped = std::tanh(sample * drive[static_cast<size_t>(index)] + bias[static_cast<size_t>(index)]);
    if (index == 1)
        shaped = 0.92f * shaped + 0.08f * sample;
    if (index == 2)
        shaped = std::copysign(std::pow(std::abs(shaped), 0.72f), shaped);
    if (index == 3)
        shaped = std::tanh(sample * 1.6f) * 0.94f + sample * 0.06f;
    return sample + (shaped - sample) * juce::jlimit(0.0f, 1.0f, mix);
}

void NorthstarMasteringAudioProcessor::applyStereoImage(
    float& left, float& right, const std::array<float, stereoBandCount>& widths,
    const std::array<int, stereoBandCount>& presets) noexcept
{
    std::array<float, stereoBandCount> leftBands {};
    std::array<float, stereoBandCount> rightBands {};
    float leftRemainder = left;
    float rightRemainder = right;
    for (int band = 0; band < stereoBandCount - 1; ++band)
    {
        const auto i = static_cast<size_t>(band);
        const auto coeff = crossoverCoefficients[i];
        stereoLowStates[0][i] = coeff * stereoLowStates[0][i] + (1.0f - coeff) * leftRemainder;
        stereoLowStates[1][i] = coeff * stereoLowStates[1][i] + (1.0f - coeff) * rightRemainder;
        leftBands[i] = stereoLowStates[0][i];
        rightBands[i] = stereoLowStates[1][i];
        leftRemainder -= leftBands[i];
        rightRemainder -= rightBands[i];
    }
    leftBands[stereoBandCount - 1] = leftRemainder;
    rightBands[stereoBandCount - 1] = rightRemainder;

    left = right = 0.0f;
    static constexpr std::array<float, stereoBandCount> presetWidth { 1.0f, 1.35f, 0.62f, 1.16f };
    for (int band = 0; band < stereoBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        // Presets change only the amount of side signal (no waveshaper per band), so
        // a neutral 100 % setting reconstructs the input exactly.
        const auto scale = presetWidth[static_cast<size_t>(juce::jlimit(0, 3, presets[i]))];
        const auto mid = (leftBands[i] + rightBands[i]) * 0.5f;
        const auto side = (leftBands[i] - rightBands[i]) * widths[i] * scale;
        left += mid + side;
        right += mid - side;
    }
}

void NorthstarMasteringAudioProcessor::finishAnalysis()
{
    const auto samples = std::max<int64_t>(1, analyzedSamples);
    const auto meanEnergy = std::max(analysisEnergy / static_cast<double>(samples), 1.0e-12);
    const auto inputLufs = static_cast<float>(-0.691 + 10.0 * std::log10(meanEnergy));
    // Kept separately from the live meter: the live meter is overwritten every block,
    // which used to make the "learned" loudness disappear right after the analysis.
    learnedLufs.store(inputLufs);
    analysisComplete.store(true);
    analysisRunning.store(false);
    analysisProgress.store(1.0f);
    collectingAnalysis = false;

    const auto totalBandEnergy = std::max(
        analysisBandEnergy[0] + analysisBandEnergy[1] + analysisBandEnergy[2], 1.0e-12);
    const auto targetShares = std::array<float, 3> { 0.25f, 0.55f, 0.20f };
    const auto isAutomatic = p.eqMode->load() < 0.5f;
    for (int index = 0; index < 3 && isAutomatic; ++index)
    {
        const auto observed = static_cast<float>(analysisBandEnergy[static_cast<size_t>(index)]
                                                  / totalBandEnergy);
        const auto correction = juce::jlimit(-5.0f, 5.0f,
            10.0f * std::log10(targetShares[static_cast<size_t>(index)]
                                / std::max(observed, 0.02f)));
        const auto band = index == 0 ? 0 : (index == 1 ? 6 : 12);
        setEQGain(band, correction * 0.38f);
    }
    if (isAutomatic)
        parameters.getParameter("eqMode")->setValueNotifyingHost(0.0f);
}

void NorthstarMasteringAudioProcessor::pushSpectrumSample(float sample) noexcept
{
    spectrumRing[static_cast<size_t>(spectrumWritePosition)] = sample;
    spectrumWritePosition = (spectrumWritePosition + 1) & (spectrumSize - 1);
    if (++spectrumSinceHop >= spectrumHop)
    {
        spectrumSinceHop = 0;
        computeSpectrum();
    }
}

void NorthstarMasteringAudioProcessor::computeSpectrum() noexcept
{
    const auto sampleRate = currentSampleRate.load();
    if (sampleRate <= 0.0)
        return;

    for (int i = 0; i < spectrumSize; ++i)
        spectrumWork[static_cast<size_t>(i)] =
            spectrumRing[static_cast<size_t>((spectrumWritePosition + i) & (spectrumSize - 1))];
    std::fill(spectrumWork.begin() + spectrumSize, spectrumWork.end(), 0.0f);
    spectrumWindow.multiplyWithWindowingTable(spectrumWork.data(), spectrumSize);
    spectrumFft.performFrequencyOnlyForwardTransform(spectrumWork.data());

    // A full-scale sine with a Hann window peaks at N/4, so this reads in dBFS.
    const auto norm = 4.0f / static_cast<float>(spectrumSize);
    const auto binHz = sampleRate / spectrumSize;
    constexpr int lastBin = spectrumSize / 2;

    std::array<float, spectrumBinCount> raw {};
    for (int i = 0; i < spectrumBinCount; ++i)
    {
        const auto lo = 20.0 * std::pow(1000.0, static_cast<double>(i) / spectrumBinCount) / binHz;
        const auto hi = 20.0 * std::pow(1000.0, static_cast<double>(i + 1) / spectrumBinCount) / binHz;
        float magnitude = 0.0f;
        if (hi - lo < 1.0)
        {
            // Display bin narrower than an FFT bin (low end): interpolate.
            const auto position = 0.5 * (lo + hi);
            const auto i0 = juce::jlimit(0, lastBin, static_cast<int>(std::floor(position)));
            const auto i1 = juce::jmin(lastBin, i0 + 1);
            const auto fraction = static_cast<float>(position - i0);
            magnitude = spectrumWork[static_cast<size_t>(i0)] * (1.0f - fraction)
                      + spectrumWork[static_cast<size_t>(i1)] * fraction;
        }
        else
        {
            const auto first = juce::jlimit(0, lastBin, static_cast<int>(std::ceil(lo)));
            const auto last = juce::jlimit(first, lastBin, static_cast<int>(std::floor(hi)));
            for (int k = first; k <= last; ++k)
                magnitude = std::max(magnitude, spectrumWork[static_cast<size_t>(k)]);
        }
        raw[static_cast<size_t>(i)] = 20.0f * std::log10(std::max(magnitude * norm, 1.0e-6f));
    }

    for (int i = 0; i < spectrumBinCount; ++i)
    {
        const auto previous = raw[static_cast<size_t>(juce::jmax(0, i - 1))];
        const auto next = raw[static_cast<size_t>(juce::jmin(spectrumBinCount - 1, i + 1))];
        const auto target = 0.25f * previous + 0.5f * raw[static_cast<size_t>(i)] + 0.25f * next;
        auto& state = spectrumSmooth[static_cast<size_t>(i)];
        state += (target - state) * (target > state ? 0.55f : 0.12f);
        spectrumBins[static_cast<size_t>(i)].store(state);
    }
}

void NorthstarMasteringAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();
    const auto channels = juce::jmin(getTotalNumInputChannels(), 2);
    for (int channel = channels; channel < getTotalNumOutputChannels(); ++channel)
        buffer.clear(channel, 0, numSamples);
    if (channels == 0 || numSamples == 0)
        return;

    if (dryBuffer.getNumSamples() < numSamples)
        dryBuffer.setSize(2, numSamples, false, false, true);

    const auto sampleRate = currentSampleRate.load();
    const auto sampleRateF = static_cast<float>(sampleRate);

    if (analysisRequested.exchange(false))
    {
        collectingAnalysis = true;
        analyzedSamples = 0;
        analysisEnergy = 0.0;
        analysisPeak = 0.0;
        analysisBandEnergy = {};
        analysisProgress.store(0.0f);
        analysisComplete.store(false);
        analysisRunning.store(true);
    }

    updateCrossovers();

    const auto bypass = p.bypass->load() > 0.5f;
    const auto eqOn = p.eqEnabled->load() > 0.5f;
    const auto eqDynamicGlobal = p.eqDynamic->load() > 0.5f;

    // Dynamic EQ detectors (side-chain only).
    std::array<bool, eqBandCount> detectBand {};
    bool anyDetector = false;
    for (int band = 0; band < eqBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        detectBand[i] = eqOn && (eqDynamicGlobal || p.eqDyn[i]->load() > 0.5f);
        if (detectBand[i])
        {
            anyDetector = true;
            dynamicDetectors[i].setBandpass(
                sampleRate, juce::jlimit(20.0, sampleRate * 0.45, static_cast<double>(p.eqFreq[i]->load())),
                juce::jmax(0.3, static_cast<double>(p.eqQ[i]->load())));
        }
        else
        {
            dynamicLevels[i] = 0.0f;
        }
    }
    const auto detectorRelease = static_cast<float>(std::exp(-1.0 / (0.08 * sampleRate)));

    // Saturation module.
    const auto saturationOn = p.saturationEnabled->load() > 0.5f;
    const auto saturationMix = saturationOn ? p.saturationMix->load() * 0.01f : 0.0f;
    const auto saturationPreset = static_cast<int>(p.saturationPreset->load());

    // Stereo module.
    const auto stereoOn = p.stereoEnabled->load() > 0.5f;
    std::array<float, stereoBandCount> stereoWidths {};
    std::array<int, stereoBandCount> stereoPresets {};
    for (int band = 0; band < stereoBandCount; ++band)
    {
        const auto i = static_cast<size_t>(band);
        stereoWidths[i] = p.width[i]->load() * 0.01f;
        stereoPresets[i] = static_cast<int>(p.preset[i]->load());
    }

    // Compressor module. When it is off, input gain, makeup gain and detector are all skipped.
    const auto compressorOn = p.compEnabled->load() > 0.5f;
    const auto compInputGain = juce::Decibels::decibelsToGain(p.compInput->load());
    const auto compOutputGain = juce::Decibels::decibelsToGain(p.compOutput->load());
    const auto compThreshold = p.compThreshold->load();
    const auto compRatio = juce::jmax(1.0f, p.compRatio->load());
    const auto attackCoeff = std::exp(-1.0f / (sampleRateF * p.compAttack->load() * 0.001f));
    const auto releaseCoeff = std::exp(-1.0f / (sampleRateF * p.compRelease->load() * 0.001f));

    // Loudness module (learned make-up gain + clean volume). Metering keeps running when off.
    const auto loudnessOn = p.loudnessEnabled->load() > 0.5f;
    float loudnessGain = 1.0f;
    if (loudnessOn)
    {
        const auto learnedMakeup = analysisComplete.load()
            ? juce::jlimit(-12.0f, 12.0f, (p.targetLufs->load() - learnedLufs.load()) * 0.72f)
            : 0.0f;
        loudnessGain = juce::Decibels::decibelsToGain(learnedMakeup)
                     * juce::Decibels::decibelsToGain(p.volume->load());
    }

    double blockInputEnergy = 0.0;
    double blockOutputEnergy = 0.0;
    float blockPeak = 0.0f;
    float blockReduction = 0.0f;

    // ---- Pass A: input metering, analysis, spectrum, saturation, dynamic detectors ----
    for (int sample = 0; sample < numSamples; ++sample)
    {
        float left = buffer.getSample(0, sample);
        float right = channels > 1 ? buffer.getSample(1, sample) : left;

        dryDelay.pushSample(0, left);
        dryDelay.pushSample(1, right);
        dryBuffer.setSample(0, sample, dryDelay.popSample(0));
        dryBuffer.setSample(1, sample, dryDelay.popSample(1));

        const auto inputMono = (left + right) * 0.5f;
        const auto detector = juce::jmax(std::abs(left), std::abs(right));
        auto weightedLeft = loudnessFilters[0][0].processSample(left);
        weightedLeft = loudnessFilters[1][0].processSample(weightedLeft);
        auto weightedRight = loudnessFilters[0][1].processSample(right);
        weightedRight = loudnessFilters[1][1].processSample(weightedRight);
        const auto weightedEnergy = 0.5f * (weightedLeft * weightedLeft + weightedRight * weightedRight);
        blockInputEnergy += weightedEnergy;
        if (collectingAnalysis)
        {
            analysisEnergy += weightedEnergy;
            analysisPeak = std::max(analysisPeak, static_cast<double>(detector));
            const auto low = detector * 0.55f;
            const auto mid = detector * 0.35f;
            const auto high = detector * 0.20f;
            analysisBandEnergy[0] += low * low;
            analysisBandEnergy[1] += mid * mid;
            analysisBandEnergy[2] += high * high;
            ++analyzedSamples;
        }

        pushSpectrumSample(inputMono);

        if (saturationMix > 0.0001f)
        {
            left = applySaturation(left, saturationMix, saturationPreset);
            right = applySaturation(right, saturationMix, saturationPreset);
        }

        if (anyDetector)
        {
            const auto mono = 0.5 * (static_cast<double>(left) + static_cast<double>(right));
            for (size_t band = 0; band < static_cast<size_t>(eqBandCount); ++band)
            {
                if (!detectBand[band])
                    continue;
                const auto level = static_cast<float>(std::abs(dynamicDetectors[band].process(mono)));
                dynamicLevels[band] = level > dynamicLevels[band]
                    ? level : dynamicLevels[band] * detectorRelease;
            }
        }

        buffer.setSample(0, sample, left);
        if (channels > 1)
            buffer.setSample(1, sample, right);
    }

    // Envelope for the dynamic nodes; the message-thread timer turns it into a new FIR.
    const auto blockSeconds = static_cast<float>(numSamples) / sampleRateF;
    const auto attackStep = 1.0f - std::exp(-blockSeconds / 0.02f);
    const auto releaseStep = 1.0f - std::exp(-blockSeconds / 0.20f);
    for (size_t band = 0; band < static_cast<size_t>(eqBandCount); ++band)
    {
        auto envelope = dynamicEnvelopes[band].load();
        const auto target = detectBand[band]
            ? juce::jlimit(0.0f, 1.0f, (toDb(dynamicLevels[band]) + 36.0f) / 12.0f) : 0.0f;
        envelope += (target - envelope) * (target > envelope ? attackStep : releaseStep);
        dynamicEnvelopes[band].store(envelope);
    }

    // ---- Pass B: linear-phase EQ (constant delay, zero phase distortion) ----
    {
        juce::dsp::AudioBlock<float> block(buffer);
        auto used = block.getSubsetChannelBlock(0, static_cast<size_t>(channels));
        eqConvolution.process(juce::dsp::ProcessContextReplacing<float>(used));
    }

    // ---- Pass C: stereo imager, compressor, gain, output metering ----
    for (int sample = 0; sample < numSamples; ++sample)
    {
        float left = buffer.getSample(0, sample);
        float right = channels > 1 ? buffer.getSample(1, sample) : left;

        if (stereoOn)
            applyStereoImage(left, right, stereoWidths, stereoPresets);

        float compGain = 1.0f;
        if (compressorOn)
        {
            left *= compInputGain;
            right *= compInputGain;
            const auto level = juce::jmax(std::abs(left), std::abs(right));
            compressorEnvelope = level > compressorEnvelope
                ? attackCoeff * compressorEnvelope + (1.0f - attackCoeff) * level
                : releaseCoeff * compressorEnvelope + (1.0f - releaseCoeff) * level;
            const auto over = juce::jmax(0.0f, toDb(compressorEnvelope) - compThreshold);
            const auto reductionDb = over * (1.0f - 1.0f / compRatio);
            compGain = juce::Decibels::decibelsToGain(-reductionDb) * compOutputGain;
            blockReduction = juce::jmax(blockReduction, reductionDb);
        }
        else
        {
            compressorEnvelope = 0.0f;
        }

        const auto outputGain = compGain * loudnessGain;
        left *= outputGain;
        right *= outputGain;

        // Float path stays transparent (no clipper); the meter reports the real peak.
        blockOutputEnergy += 0.5 * (left * left + right * right);
        blockPeak = juce::jmax(blockPeak, std::abs(left), std::abs(right));
        buffer.setSample(0, sample, left);
        if (channels > 1)
            buffer.setSample(1, sample, right);
    }

    // Bypass = the input delayed by the same latency as the processed path.
    if (bypass)
        for (int channel = 0; channel < channels; ++channel)
            buffer.copyFrom(channel, 0, dryBuffer, channel, 0, numSamples);

    const auto sampleCount = juce::jmax(1, numSamples);
    const auto inputLufs = -0.691f + 10.0f * std::log10(
        std::max(static_cast<float>(blockInputEnergy / sampleCount), 1.0e-12f));
    const auto outputLufs = -0.691f + 10.0f * std::log10(
        std::max(static_cast<float>(blockOutputEnergy / sampleCount), 1.0e-12f));
    smoothInputLoudness = smoothInputLoudness < -59.0f
        ? inputLufs : 0.92f * smoothInputLoudness + 0.08f * inputLufs;
    smoothOutputLoudness = smoothOutputLoudness < -59.0f
        ? outputLufs : 0.92f * smoothOutputLoudness + 0.08f * outputLufs;
    smoothPeak = smoothPeak < -59.0f
        ? toDb(blockPeak) : 0.84f * smoothPeak + 0.16f * toDb(blockPeak);
    loudnessEstimate.store(smoothInputLoudness);
    outputLoudness.store(smoothOutputLoudness);
    peakDb.store(smoothPeak);
    gainReductionDb.store(blockReduction);

    if (collectingAnalysis)
    {
        analysisProgress.store(juce::jlimit(0.0f, 1.0f,
            static_cast<float>(analyzedSamples)
            / static_cast<float>(sampleRate * analysisDurationSeconds)));
        if (analyzedSamples >= static_cast<int64_t>(sampleRate * analysisDurationSeconds))
            finishAnalysis();
    }
}

void NorthstarMasteringAudioProcessor::copySpectrum(
    std::array<float, spectrumBinCount>& destination) const noexcept
{
    for (size_t index = 0; index < destination.size(); ++index)
        destination[index] = spectrumBins[index].load();
}

float NorthstarMasteringAudioProcessor::getEQFrequency(int band) const noexcept
{
    return p.eqFreq[static_cast<size_t>(juce::jlimit(0, eqBandCount - 1, band))]->load();
}

float NorthstarMasteringAudioProcessor::getEQGain(int band) const noexcept
{
    return p.eqGain[static_cast<size_t>(juce::jlimit(0, eqBandCount - 1, band))]->load();
}

float NorthstarMasteringAudioProcessor::getEQQ(int band) const noexcept
{
    return p.eqQ[static_cast<size_t>(juce::jlimit(0, eqBandCount - 1, band))]->load();
}

bool NorthstarMasteringAudioProcessor::getEQDynamic(int band) const noexcept
{
    return p.eqDyn[static_cast<size_t>(juce::jlimit(0, eqBandCount - 1, band))]->load() > 0.5f;
}

float NorthstarMasteringAudioProcessor::getEQEffectiveGain(int band) const noexcept
{
    const auto i = static_cast<size_t>(juce::jlimit(0, eqBandCount - 1, band));
    const auto dynamic = p.eqDynamic->load() > 0.5f || p.eqDyn[i]->load() > 0.5f;
    return effectiveGain(p.eqGain[i]->load(), dynamic, dynamicEnvelopes[i].load());
}

void NorthstarMasteringAudioProcessor::setEQFrequency(int band, float value)
{
    const auto index = juce::jlimit(0, eqBandCount - 1, band);
    auto* parameter = parameters.getParameter(eqId("eqFreq", index));
    parameter->setValueNotifyingHost(parameter->convertTo0to1(juce::jlimit(20.0f, 20000.0f, value)));
}

void NorthstarMasteringAudioProcessor::setEQGain(int band, float value)
{
    const auto index = juce::jlimit(0, eqBandCount - 1, band);
    auto* parameter = parameters.getParameter(eqId("eqGain", index));
    parameter->setValueNotifyingHost(parameter->convertTo0to1(juce::jlimit(-18.0f, 18.0f, value)));
}

void NorthstarMasteringAudioProcessor::setEQQ(int band, float value)
{
    const auto index = juce::jlimit(0, eqBandCount - 1, band);
    auto* parameter = parameters.getParameter(eqId("eqQ", index));
    parameter->setValueNotifyingHost(parameter->convertTo0to1(juce::jlimit(0.1f, 12.0f, value)));
}

void NorthstarMasteringAudioProcessor::setEQDynamic(int band, bool enabled)
{
    const auto index = juce::jlimit(0, eqBandCount - 1, band);
    parameters.getParameter(eqId("eqDyn", index))->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
}

void NorthstarMasteringAudioProcessor::resetEQToAuto()
{
    for (int band = 0; band < eqBandCount; ++band)
    {
        setEQGain(band, 0.0f);
        setEQDynamic(band, false);
    }
    parameters.getParameter("eqMode")->setValueNotifyingHost(0.0f);
}

void NorthstarMasteringAudioProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destinationData);
}

void NorthstarMasteringAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
}

juce::AudioProcessorEditor* NorthstarMasteringAudioProcessor::createEditor()
{
    return new NorthstarMasteringAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NorthstarMasteringAudioProcessor();
}
