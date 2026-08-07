# ESP32 Audio Quality Testing Pipeline

Objective, repeatable metrics to track whether your ESP32 recording settings are improving or degrading audio that Whisper sees.

## Overview

This testing pipeline provides:

1. **Fixed test corpus** - 20 standardized utterances with gold transcriptions
2. **WER evaluation** - Word Error Rate using `faster-whisper large-v3`
3. **Signal metrics** - RMS, clipping ratio, SNR estimate, spectral analysis
4. **Experiment tracking** - Track configurations over time and compare results

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌──────────────────────┐
│   ESP32-S3      │────>│  speech-to-speech│────>│ faster-whisper (16kHz)│
└─────────────────┘     └──────────────────┘     └──────────────────────┘
         │                                                  │
         ▼                                                  ▼
┌────────────────────────────────────────────────────────────────────┐
│                  Testing Pipeline                                  │
├────────────────────────────────────────────────────────────────────┤
│  • Fixed test corpus (gold transcripts)                            │
│  • WER computation (jiwer + faster-whisper)                        │
│  • Signal quality metrics                                          │
│  • Experiment tracking & comparison                                │
└────────────────────────────────────────────────────────────────────┘
```

## Quick Start

```bash
# Install dependencies
pip install -r testing/requirements.txt

# Create gold recordings (once)
python testing/record_gold_corpus.py

# Record test utterances with your ESP32 config
# Save WAV files as: test_configs/config_name/utt_001.wav, utt_002.wav, etc.

# Evaluate WER for a config
python testing/evaluate_wer.py --config-dir test_configs/config_v1

# Track and compare multiple configs
python testing/experiment_tracker.py add --name config_v1 --results results/config_v1_results.json
python testing/experiment_tracker.py list
python testing/experiment_tracker.py compare --output plots/comparison.png
```

## Directory Structure

```
testing/
├── README.md                    # This file
├── requirements.txt             # Python dependencies
├── evaluate_wer.py              # Main WER evaluation script
├── record_gold_corpus.py        # Record gold reference corpus
├── analyze_esp32_config.py      # Analyze ESP32 I2S configuration
├── experiment_tracker.py        # Track experiments over time
├── run_experiment.sh/bat        # Run all configs (shell/Batch)
│
├── test_corpus/
│   ├── gold_transcripts.txt     # 20 fixed utterances with ground truth
│   └── README.md                # Test corpus documentation
│
└── test_configs/
    ├── config_v1/               # ESP32 recording config v1
    │   ├── utt_001.wav
    │   ├── utt_002.wav
    │   └── ...
    ├── config_v2/               # Config v2 (for comparison)
    └── ...
```

## Usage Guide

### 1. Create Gold Transcription Corpus

Run once to create reference recordings:

```bash
python testing/record_gold_corpus.py
```

**Instructions:**
- Use a high-quality microphone (USB mic, phone recorder, etc.)
- Speak each utterance clearly at consistent volume
- Leave ~0.5s silence before and after speech
- Save as `utt_001.wav`, `utt_002.wav`, etc.

The gold corpus is **never changed** - it's your reference standard.

### 2. Record Test Utterances

For each ESP32 configuration you want to test:

```bash
# Configure your ESP32 with the desired I2S/PDM settings
# Run the test and save recordings to:
test_configs/config_name/utt_001.wav
test_configs/config_name/utt_002.wav
...
```

**Important:** Keep everything else constant:
- Same recording device (ESP32)
- Same distance from speaker
- Same room/environment
- Only change the ESP32 configuration being tested

### 3. Evaluate WER and Signal Metrics

```bash
python testing/evaluate_wer.py \
    --config-dir test_configs/config_v1 \
    --transcripts test_corpus/gold_transcripts.txt \
    --output-dir results \
    --model-size large-v3
```

**Output includes:**
- Word Error Rate (WER)
- Accuracy percentage
- RMS level (loudness)
- Clipping ratio (distortion indicator)
- SNR estimate (signal quality)
- Spectral analysis

### 4. Track Experiments Over Time

```bash
# Add results to experiment history
python testing/experiment_tracker.py add \
    --name config_v1 \
    --results results/config_v1_results.json \
    --notes "Gain=0dB, I2S 16-bit"

python testing/experiment_tracker.py add \
    --name config_v2 \
    --results results/config_v2_results.json \
    --notes "Gain=-3dB, I2S 16-bit"
```

**View experiment history:**
```bash
python testing/experiment_tracker.py list
```

**Generate comparison plot:**
```bash
python testing/experiment_tracker.py compare --output plots/comparison.png
```

## Signal Quality Metrics

### WER (Word Error Rate)
- **Primary metric** for ASR quality
- Lower is better (0% = perfect)
- Directly measures how well Whisper understands your audio

### RMS Level
- Measures overall loudness
- Target: ~0.3 to 0.6 (-10 to -4 dBFS)
- Too low → Whisper underdriven, more errors
- Too high → potential clipping/distortion

### Clipping Ratio
- Fraction of samples at max/min amplitude
- Target: <1% (ideally near 0%)
- >5% indicates severe distortion

### SNR Estimate
- Signal-to-noise ratio in dB
- Target: >20 dB for good ASR
- Higher = better signal quality

### Spectral Analysis
- Energy distribution across frequency bands
- Speech band: 300-8000 Hz (重点区域)
- Look for drops in mid/high frequencies (indicates muffled speech)

## ESP32 Configuration Parameters

### Sample Rate & Bit Depth
```cpp
// Target for Whisper: 16kHz, 16-bit, mono
I2S.setRate(16000);      // Sample rate
I2S.setBitsPerSample(16); // Bit depth
I2S.setChannels(1);       // Mono
```

### Gain Settings
```cpp
// Aim for peak levels around -6 to -3 dBFS
// No clipping on loud speech
micGain = 0;    // Try values: -6, -3, 0, +3
```

### I2S/PDM Configuration
- Confirm I2S pins match your hardware
- Use mono (not stereo) for Whisper
- Check decimation filter settings for PDM microphones

## Experimental Protocol

1. **Baseline**: Record with working configuration → measure WER
2. **Change one parameter** at a time:
   - Sample rate: 16kHz vs 48kHz
   - Bit depth: 16-bit vs 32-bit
   - Gain: -6dB vs -3dB vs 0dB
   - I2S mode: PDM vs standard I2S
3. **Record all utterances** for each config
4. **Evaluate WER** and log results
5. **Compare metrics**: Look for correlated improvements

## Example Workflow

```bash
# 1. Create gold corpus (once)
python testing/record_gold_corpus.py

# 2. Test original configuration
cp test_configs/config_original/utt_*.wav temp/
# Record new utterances with ESP32...
mv temp/*.wav test_configs/config_v0/

# 3. Evaluate baseline
python testing/evaluate_wer.py --config-dir test_configs/config_v0

# 4. Adjust gain on ESP32 and test again
python testing/evaluate_wer.py --config-dir test_configs/config_v1

# 5. Track and compare
python testing/experiment_tracker.py add --name v0 --results results/v0_results.json
python testing/experiment_tracker.py add --name v1 --results results/v1_results.json
python testing/experiment_tracker.py list
```

## Troubleshooting

### "faster-whisper model not found"
```bash
pip install faster-whisper>=0.10.0
```

### "sounddevice not found" (for recording)
```bash
pip install sounddevice soundfile
```

### High WER on all configs?
- Check sample rate conversion (must be 16kHz mono)
- Verify no heavy DSP between ESP32 and Whisper
- Ensure consistent volume in recordings

## Extending the Pipeline

### Add New Metrics
Edit `evaluate_wer.py` to add:
- Perceptual evaluation (PESQ)
- STOI (speech intelligibility)
- MOS (mean opinion score) estimates

### Automate ESP32 Testing
Use ESP-IDF to programmatically test multiple configurations:

```python
# Pseudocode for automated testing
for gain in [-6, -3, 0, 3]:
    for rate in [16000, 48000]:
        configure_esp32(gain=gain, rate=rate)
        record_test_corpus()
        evaluate_wer()
```

## References

- Whisper models: https://github.com/SYSTRAN/faster-whisper
- ESP32 audio front-end design: https://docs.espressif.com/projects/esp-sr/en/latest/esp32/audio_front_end/
- I2S configuration: https://geo-tp.github.io/ESP32-Bit-Pirate/protocols/i2s/
