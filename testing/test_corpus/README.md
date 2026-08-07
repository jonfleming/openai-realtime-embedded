# Test Corpus

This directory contains the fixed test corpus used for ESP32 audio recording evaluation.

## Components

1. **gold_transcripts.txt** - 20 short utterances with ground truth transcriptions
2. **gold_recordings/** - Directory for gold recordings (created once, then never changed)
3. **test_configs/** - Subdirectories for each ESP32 configuration test

## Usage

1. Create gold recordings by playing each utterance through a reference speaker and recording with a high-quality microphone
2. Save each as `utt_001.wav`, `utt_002.wav`, etc. in `gold_recordings/`
3. For each ESP32 config, record all utterances and save to `test_configs/config_name/`

## Metrics

Each test run produces:
- **WER** (Word Error Rate) - primary ASR quality metric
- **RMS level** - overall loudness
- **Clipping ratio** - percentage of samples at max/min values
- **SNR estimate** - signal-to-noise ratio
- **Spectral analysis** - frequency energy distribution
