#!/usr/bin/env python3
"""
Audio Quality Evaluation Script for ESP32 Testing Pipeline

Computes WER and signal quality metrics for audio recordings.
Designed to work with faster_whisper large-v3 on 16kHz mono audio.

Usage:
    # Evaluate a single config directory
    python evaluate_wer.py --config-dir test_configs/config_v1
    
    # Compare multiple configs and generate summary table
    python evaluate_wer.py --compare-configs config_v1 config_v2 config_v3
    
    # Generate spectral analysis plots
    python evaluate_wer.py --spectral-analysis config_v1

Dependencies:
    faster-whisper>=0.10.0
    jiwer
    numpy
    scipy
    soundfile
    matplotlib (optional, for plots)
"""

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Tuple, Optional

import numpy as np
import soundfile as sf
from faster_whisper import WhisperModel
import jiwer


# Constants
TARGET_SAMPLE_RATE = 16000
SPEECH_BAND_MIN = 300
SPEECH_BAND_MAX = 8000
CLIPPING_THRESHOLD = 0.95  # Fraction of max amplitude


def load_gold_transcripts(transcript_path: Path) -> Dict[str, str]:
    """Load gold transcripts from text file."""
    transcripts = {}
    with open(transcript_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('|', 1)
            if len(parts) == 2:
                utt_id, text = parts
                transcripts[utt_id] = text
    return transcripts


def transcribe_audio(model: WhisperModel, audio_path: Path) -> str:
    """Transcribe a single audio file using faster_whisper."""
    segments, _ = model.transcribe(
        str(audio_path),
        language="en",
        word_timestamps=False,
        vad_filter=True,
        vad_parameters=dict(min_silence_duration_ms=500)
    )
    return " ".join(segment.text for segment in segments).strip()


def compute_wer(reference: str, hypothesis: str) -> Tuple[float, Dict]:
    """Compute Word Error Rate and detailed metrics."""
    reference = reference.lower().strip()
    hypothesis = hypothesis.lower().strip()
    
    measures = jiwer.compute_measures(reference, hypothesis)
    
    wer = measures['wer']
    correct_words = measures['sub'] + measures['ins'] + measures['del'] + measures['corr']
    if correct_words > 0:
        accuracy = (measures['corr'] / correct_words) * 100
    else:
        accuracy = 0.0
    
    return wer, {
        'wer': wer,
        'accuracy_pct': accuracy,
        'substitutions': measures['sub'],
        'insertions': measures['ins'],
        'deletions': measures['del'],
        'correct': measures['corr'],
        'reference_len': len(reference.split()),
        'hypothesis_len': len(hypothesis.split())
    }


def compute_rms(audio: np.ndarray) -> float:
    """Compute RMS level of audio signal."""
    return np.sqrt(np.mean(audio ** 2))


def compute_clipping_ratio(audio: np.ndarray, dtype=np.int16) -> float:
    """Compute fraction of samples at clipping threshold."""
    max_val = np.iinfo(dtype).max
    clipped = np.sum(np.abs(audio) == max_val)
    total = len(audio)
    return clipped / total


def compute_snr_estimate(
    audio: np.ndarray, 
    sample_rate: int,
    speech_threshold: float = 0.01
) -> Tuple[float, Dict]:
    """
    Estimate SNR by separating speech and silence segments.
    
    Uses energy-based voice activity detection to estimate noise floor.
    """
    # Compute frame-wise energy
    frame_size = int(sample_rate * 0.025)  # 25ms frames
    hop_size = int(sample_rate * 0.010)   # 10ms hop
    
    energy = []
    for i in range(0, len(audio) - frame_size, hop_size):
        frame = audio[i:i + frame_size]
        energy.append(np.sum(frame ** 2))
    
    energy = np.array(energy)
    
    # Estimate noise floor (use lower quartile as noise estimate)
    noise_energy = np.percentile(energy, 25)
    speech_energy = np.mean(energy[energy > noise_threshold * noise_energy])
    
    # Convert to dB
    if noise_energy > 0 and speech_energy > 0:
        snr_db = 10 * np.log10(speech_energy / noise_energy)
    else:
        snr_db = 0.0
    
    return snr_db, {
        'noise_floor': np.sqrt(noise_energy),
        'speech_level': np.sqrt(speech_energy) if speech_energy > 0 else 0,
        'snr_db': snr_db
    }


def compute_spectral_analysis(
    audio: np.ndarray, 
    sample_rate: int,
    n_fft: int = 2048
) -> Dict:
    """Compute frequency spectrum analysis."""
    # Compute FFT
    window = np.hanning(len(audio))
    audio_windowed = audio * window
    
    fft_vals = np.fft.rfft(audio_windowed, n=n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1/sample_rate)
    
    # Magnitude spectrum
    magnitude = np.abs(fft_vals)
    
    # Compute energy in speech bands
    speech_band_mask = (freqs >= SPEECH_BAND_MIN) & (freqs <= SPEECH_BAND_MAX)
    total_energy = np.sum(magnitude ** 2)
    speech_energy = np.sum(magnitude[speech_band_mask] ** 2)
    
    if total_energy > 0:
        speech_band_ratio = speech_energy / total_energy
    else:
        speech_band_ratio = 0.0
    
    # Find dominant frequency bands
    freq_resolution = sample_rate / n_fft
    low_band = np.sum(magnitude[(freqs >= 300) & (freqs < 800)] ** 2)
    mid_band = np.sum(magnitude[(freqs >= 800) & (freqs < 2500)] ** 2)
    high_band = np.sum(magnitude[(freqs >= 2500) & (freqs <= 8000)] ** 2)
    
    total = low_band + mid_band + high_band
    if total > 0:
        band_ratios = {
            '300-800 Hz': low_band / total,
            '800-2500 Hz': mid_band / total,
            '2500-8000 Hz': high_band / total
        }
    else:
        band_ratios = {'300-800 Hz': 0, '800-2500 Hz': 0, '2500-8000 Hz': 0}
    
    return {
        'total_energy': total_energy,
        'speech_band_ratio': speech_band_ratio,
        'band_ratios': band_ratios,
        'spectral_centroid': float(np.sum(freqs * magnitude) / np.sum(magnitude)) if np.sum(magnitude) > 0 else 0
    }


def analyze_audio_file(audio_path: Path, sample_rate: int = TARGET_SAMPLE_RATE) -> Dict:
    """Comprehensive audio analysis for a single file."""
    audio, sr = sf.read(audio_path)
    
    # Resample if needed
    if sr != sample_rate:
        from scipy import signal
        num_samples = int(len(audio) * sample_rate / sr)
        audio = signal.resample(audio, num_samples)
        sr = sample_rate
    
    # Convert to mono if stereo
    if len(audio.shape) > 1:
        audio = np.mean(audio, axis=1)
    
    # Normalize for consistent RMS comparison (target: -6 dBFS ≈ 0.5 RMS)
    target_rms = 0.5
    current_rms = compute_rms(audio)
    if current_rms > 0:
        audio = audio * (target_rms / current_rms)
    
    # Compute all metrics
    rms = compute_rms(audio)
    clipping = compute_clipping_ratio(audio)
    snr_db, snr_details = compute_snr_estimate(audio, sr)
    spectral = compute_spectral_analysis(audio, sr)
    
    return {
        'path': str(audio_path),
        'sample_rate': sr,
        'duration_sec': len(audio) / sr,
        'rms_level': rms,
        'clipping_ratio': clipping * 100,  # Convert to percentage
        'snr_db': snr_db,
        **snr_details,
        **spectral
    }


def evaluate_config(
    config_dir: Path,
    transcripts: Dict[str, str],
    model: WhisperModel,
    output_path: Optional[Path] = None
) -> Dict:
    """Evaluate WER for all utterances in a config directory."""
    results = {
        'config': config_dir.name,
        'utterances': [],
        'summary': {}
    }
    
    total_wer = 0.0
    count = 0
    
    # Find all .wav files sorted by name
    wav_files = sorted(config_dir.glob('utt_*.wav'))
    
    for wav_path in wav_files:
        utt_id = wav_path.stem
        
        if utt_id not in transcripts:
            print(f"Warning: No transcript found for {utt_id}")
            continue
        
        # Transcribe
        hypothesis = transcribe_audio(model, wav_path)
        reference = transcripts[utt_id]
        
        # Compute WER
        wer, details = compute_wer(reference, hypothesis)
        
        # Audio analysis
        audio_metrics = analyze_audio_file(wav_path)
        
        result = {
            'utterance_id': utt_id,
            'reference': reference,
            'hypothesis': hypothesis,
            'wer': wer,
            **details,
            **audio_metrics
        }
        
        results['utterances'].append(result)
        total_wer += wer
        count += 1
    
    if count > 0:
        results['summary']['average_wer'] = total_wer / count
        results['summary']['total_utterances'] = count
        
        # Aggregate metrics
        avg_rms = np.mean([u.get('rms_level', 0) for u in results['utterances']])
        avg_clipping = np.mean([u.get('clipping_ratio', 0) for u in results['utterances']])
        avg_snr = np.mean([u.get('snr_db', 0) for u in results['utterances']])
        
        results['summary']['avg_rms'] = avg_rms
        results['summary']['avg_clipping_pct'] = avg_clipping
        results['summary']['avg_snr_db'] = avg_snr
    
    # Save results if output path provided
    if output_path:
        with open(output_path, 'w') as f:
            json.dump(results, f, indent=2)
        print(f"Results saved to {output_path}")
    
    return results


def compare_configs(
    config_dirs: List[Path],
    transcripts: Dict[str, str],
    model: WhisperModel,
    output_dir: Optional[Path] = None
) -> List[Dict]:
    """Evaluate multiple configs and compare results."""
    all_results = []
    
    for config_dir in config_dirs:
        if not config_dir.exists():
            print(f"Warning: Config directory not found: {config_dir}")
            continue
        
        output_file = output_dir / f"{config_dir.name}_results.json" if output_dir else None
        results = evaluate_config(config_dir, transcripts, model, output_file)
        all_results.append(results)
    
    return all_results


def print_comparison_table(results_list: List[Dict]) -> None:
    """Print formatted comparison table."""
    print("\n" + "="*80)
    print("ESP32 AUDIO CONFIGURATION COMPARISON")
    print("="*80)
    
    # Header
    print(f"{'Config':<20} {'WER':>8} {'Acc%':>7} {'RMS':>8} {'Clp%':>7} {'SNR dB':>8}")
    print("-"*80)
    
    for results in results_list:
        config = results.get('config', 'unknown')
        summary = results.get('summary', {})
        
        wer = summary.get('average_wer', float('nan'))
        acc = summary.get('avg_accuracy_pct', 0)
        rms = summary.get('avg_rms', 0)
        clipping = summary.get('avg_clipping_pct', 0)
        snr = summary.get('avg_snr_db', 0)
        
        print(f"{config:<20} {wer:>8.3f} {acc:>7.1f} {rms:>8.4f} {clipping:>7.2f} {snr:>8.1f}")
    
    print("="*80)


def main():
    parser = argparse.ArgumentParser(
        description='Evaluate ESP32 audio recording quality using WER and signal metrics'
    )
    parser.add_argument('--config-dir', type=Path, help='Directory containing WAV files to evaluate')
    parser.add_argument('--compare-configs', nargs='+', type=Path, 
                        help='Multiple config directories to compare')
    parser.add_argument('--transcripts', type=Path, default=None,
                        help='Path to gold transcripts file (default: test_corpus/gold_transcripts.txt)')
    parser.add_argument('--output-dir', type=Path, default=None,
                        help='Output directory for results JSON files')
    parser.add_argument('--model-size', type=str, default='large-v3',
                        help='Faster Whisper model size (default: large-v3)')
    parser.add_argument('--device', type=str, default='cuda' if __import__('torch').cuda.is_available() else 'cpu',
                        help='Device for Whisper inference (cuda/cpu)')
    parser.add_argument('--compute-type', type=str, default='float16',
                        help='Compute type for Faster Whisper')
    
    args = parser.parse_args()
    
    # Validate arguments
    if not args.config_dir and not args.compare_configs:
        parser.error('Either --config-dir or --compare-configs is required')
    
    # Load transcripts
    transcript_path = args.transcripts or Path(__file__).parent / 'test_corpus' / 'gold_transcripts.txt'
    if not transcript_path.exists():
        print(f"Error: Transcripts file not found: {transcript_path}")
        sys.exit(1)
    
    transcripts = load_gold_transcripts(transcript_path)
    print(f"Loaded {len(transcripts)} gold transcripts")
    
    # Initialize Whisper model
    print(f"Loading Faster Whisper {args.model_size} on {args.device}...")
    model = WhisperModel(
        args.model_size,
        device=args.device,
        compute_type=args.compute_type
    )
    
    output_dir = args.output_dir or Path(__file__).parent / 'results'
    output_dir.mkdir(exist_ok=True)
    
    if args.config_dir:
        # Single config evaluation
        results = evaluate_config(args.config_dir, transcripts, model, 
                                  output_dir / f"{args.config_dir.name}_results.json")
        
        print(f"\nResults for {args.config_dir.name}:")
        print(f"  Average WER: {results['summary'].get('average_wer', float('nan')):.3f}")
        print(f"  Avg RMS Level: {results['summary'].get('avg_rms', 0):.4f}")
        print(f"  Avg Clipping: {results['summary'].get('avg_clipping_pct', 0):.2f}%")
        print(f"  Avg SNR: {results['summary'].get('avg_snr_db', 0):.1f} dB")
        
    elif args.compare_configs:
        # Multiple config comparison
        results_list = compare_configs(args.compare_configs, transcripts, model, output_dir)
        print_comparison_table(results_list)


if __name__ == '__main__':
    main()
