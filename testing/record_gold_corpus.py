#!/usr/bin/env python3
"""
Record Gold Corpus Script

Records test utterances using a high-quality microphone for use as gold references.
This script only needs to be run ONCE to create the reference corpus.

Usage:
    # Record all utterances from gold_transcripts.txt
    python record_gold_corpus.py
    
    # Or specify output directory and sample rate
    python record_gold_corpus.py --output-dir test_corpus/gold_recordings --sample-rate 48000

Requirements:
    - PyAudio or sounddevice for recording
    - The script will prompt you to speak each utterance
"""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Tuple
import time

try:
    import sounddevice as sd
    import soundfile as sf
    HAS_SOUNDDEVICE = True
except ImportError:
    HAS_SOUNDDEVICE = False


def load_transcripts(transcript_path: Path) -> List[Tuple[str, str]]:
    """Load utterance IDs and text from transcripts file."""
    items = []
    with open(transcript_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('|', 1)
            if len(parts) == 2:
                utt_id, text = parts
                items.append((utt_id, text))
    return items


def record_utterance(
    duration: float = 6.0,
    sample_rate: int = 48000,
    channels: int = 1,
    device: int = None
) -> Tuple[str, sd.ndarray]:
    """
    Record a single utterance.
    
    Returns: (filename_hint, audio_data)
    """
    print(f"\nRecord for {duration}s... Press Enter to start recording")
    input()
    
    print("Recording... Speak clearly!")
    
    # Record
    recording = sd.rec(
        int(duration * sample_rate),
        samplerate=sample_rate,
        channels=channels,
        dtype='int16',
        device=device
    )
    
    sd.wait()  # Wait until recording is finished
    
    print("Recording complete.")
    
    return "recording", recording.flatten()


def save_audio(audio: sd.ndarray, output_path: Path, sample_rate: int = 48000) -> None:
    """Save audio to WAV file."""
    sf.write(output_path, audio, sample_rate)
    print(f"Saved: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Record gold reference corpus for ESP32 testing'
    )
    parser.add_argument('--transcripts', type=Path, default=None,
                        help='Path to gold transcripts file')
    parser.add_argument('--output-dir', type=Path, default='test_corpus/gold_recordings',
                        help='Output directory for recorded files')
    parser.add_argument('--sample-rate', type=int, default=48000,
                        help='Recording sample rate (default: 48000)')
    parser.add_argument('--device', type=int, default=None,
                        help='Input device ID (list with python -m sounddevice)')
    
    args = parser.parse_args()
    
    if not HAS_SOUNDDEVICE:
        print("Error: sounddevice module is required. Install with:")
        print("  pip install sounddevice soundfile")
        sys.exit(1)
    
    # Setup
    transcript_path = args.transcripts or Path(__file__).parent / 'test_corpus' / 'gold_transcripts.txt'
    output_dir = args.output_dir
    
    if not transcript_path.exists():
        print(f"Error: Transcripts file not found: {transcript_path}")
        sys.exit(1)
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    items = load_transcripts(transcript_path)
    print(f"Found {len(items)} utterances to record")
    print(f"Output directory: {output_dir}")
    print("\n=== GOLD CORPUS RECORDING ===\n")
    print("Instructions:")
    print("1. Use a high-quality microphone (USB mic, phone, etc.)")
    print("2. Speak each utterance clearly at consistent volume")
    print("3. Leave ~0.5s silence before and after speech")
    print("4. Press Enter to start recording each utterance\n")
    
    # Show available devices
    print("Available input devices:")
    for i, dev in enumerate(sd.query_devices()):
        if dev['max_input_channels'] > 0:
            print(f"  [{i}] {dev['name']} (inputs: {dev['max_input_channels']})")
    print()
    
    recorded_count = 0
    
    for utt_id, text in items:
        print(f"\n{'='*60}")
        print(f"{utt_id}: \"{text}\"")
        print(f"{'='*60}")
        
        recording_path = output_dir / f"{utt_id}.wav"
        
        if recording_path.exists():
            print(f"Already exists: {recording_path}")
            confirm = input("Record again? (y/N): ").strip().lower()
            if confirm != 'y':
                recorded_count += 1
                continue
        
        # Record
        _, audio = record_utterance(
            duration=6.0,
            sample_rate=args.sample_rate,
            channels=1,
            device=args.device
        )
        
        # Save (resample to target rate for consistency)
        if args.sample_rate != 16000:
            from scipy import signal
            num_samples = int(len(audio) * 16000 / args.sample_rate)
            audio_16k = signal.resample(audio, num_samples).astype('int16')
            save_audio(audio_16k, recording_path, sample_rate=16000)
        else:
            save_audio(audio, recording_path, sample_rate=args.sample_rate)
        
        recorded_count += 1
    
    print(f"\n{'='*60}")
    print(f"Recording complete! {recorded_count}/{len(items)} utterances recorded.")
    print(f"{'='*60}")


if __name__ == '__main__':
    main()
