#!/usr/bin/env python3
"""
ESP32 Audio Configuration Analyzer

Analyzes ESP32 I2S/PDM configuration and extracts recording parameters.
Designed to help you document and track configuration changes for testing.

Usage:
    # Analyze a specific ESP32 sketch file
    python analyze_esp32_config.py --file client_esp32/client_esp32.ino
    
    # Generate comparison table across multiple configs
    python analyze_esp32_config.py --compare config_v1.ini config_v2.ini config_v3.ini

Dependencies:
    - pathlib, re (stdlib)
"""

import argparse
import json
import os
import re
from pathlib import Path
from typing import Dict, Optional


def extract_i2s_params(content: str) -> Dict:
    """Extract I2S configuration parameters from ESP32 code."""
    params = {}
    
    # Sample rate patterns
    sample_rate_match = re.search(r'sampleRate\s*[=:]\s*(\d+)', content)
    if sample_rate_match:
        params['sample_rate'] = int(sample_rate_match.group(1))
    else:
        params['sample_rate'] = None
    
    # Bit depth patterns
    bit_depth_match = re.search(r'bitsPerSample\s*[=:]\s*(\d+)', content)
    if bit_depth_match:
        params['bit_depth'] = int(bit_depth_match.group(1))
    else:
        params['bit_depth'] = 16  # Default
    
    # Channel count
    channels_match = re.search(r'channels\s*[=:]\s*(\d+)', content)
    if channels_match:
        params['channel_count'] = int(channels_match.group(1))
    else:
        params['channel_count'] = 1  # Default mono
    
    # I2S pin configuration
    pin_patterns = {
        'bclk_pin': r'BCLK\s*[=:]\s*(\d+)',
        'din_pin': r'DIN\s*[=:]\s*(\d+)',
        'lrc_pin': r'LRC\s*[=:]\s*(\d+)',
        'dout_pin': r'DOUT\s*[=:]\s*(\d+)'
    }
    
    for name, pattern in pin_patterns.items():
        match = re.search(pattern, content)
        if match:
            params[name] = int(match.group(1))
    
    # PDM-specific settings
    pdm_match = re.search(r'PDM\s*[=:]\s*(true|false|1|0)', content, re.IGNORECASE)
    if pdm_match:
        params['is_pdm'] = pdm_match.group(1).lower() in ('true', '1')
    
    # Gain settings
    gain_patterns = [
        r'gain\s*[=:]\s*(\d+(?:\.\d+)?)',
        r'digitalGain\s*[=:]\s*(\d+(?:\.\d+)?)',
        r'micGain\s*[=:]\s*(\d+(?:\.\d+)?)'
    ]
    
    for pattern in gain_patterns:
        match = re.search(pattern, content, re.IGNORECASE)
        if match:
            params['gain_db'] = float(match.group(1))
            break
    
    # ALC (Automatic Level Control) settings
    alc_match = re.search(r'ALC|automatic.*?level', content, re.IGNORECASE)
    if alc_match:
        params['has_alc'] = True
    
    return params


def extract_digital_filters(content: str) -> Dict:
    """Extract digital filter and signal processing parameters."""
    filters = {}
    
    # Decimation filter (PDM)
    decim_match = re.search(r'decimation\s*[=:]\s*(\d+)', content, re.IGNORECASE)
    if decim_match:
        filters['decimation'] = int(decim_match.group(1))
    
    # High-pass filter
    hp_match = re.search(r'highPass|hpFilter', content, re.IGNORECASE)
    if hp_match:
        filters['has_highpass'] = True
    
    # Low-pass filter
    lp_match = re.search(r'lowPass|lpFilter', content, re.IGNORECASE)
    if lp_match:
        filters['has_lowpass'] = True
    
    return filters


def extract_gain_settings(content: str) -> Dict:
    """Extract microphone gain configuration."""
    gains = {}
    
    # AGC settings
    agc_match = re.search(r'AGC|automatic.*?gain', content, re.IGNORECASE)
    if agc_match:
        gains['has_agc'] = True
    
    # Manual gain range
    gain_range_match = re.search(r'gainRange\s*[=:]\s*\[(\d+),\s*(\d+)\]', content)
    if gain_range_match:
        gains['min_gain'] = int(gain_range_match.group(1))
        gains['max_gain'] = int(gain_range_match.group(2))
    
    return gains


def analyze_esp32_file(file_path: Path) -> Dict:
    """Analyze an ESP32 sketch file for audio configuration."""
    with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    config = {
        'file': str(file_path),
        'i2s_parameters': extract_i2s_params(content),
        'digital_filters': extract_digital_filters(content),
        'gain_settings': extract_gain_settings(content)
    }
    
    return config


def generate_comparison_table(configs: list) -> None:
    """Print formatted comparison table for multiple configs."""
    print("\n" + "="*100)
    print("ESP32 AUDIO CONFIGURATION COMPARISON")
    print("="*100)
    
    # Headers
    headers = [
        'Config', 'Sample Rate', 'Bit Depth', 'Channels',
        'Gain (dB)', 'Is PDM', 'Decimation'
    ]
    
    header_format = "{:<25} {:<12} {:<10} {:<9} {:<10} {:<8} {:<12}"
    print(header_format.format(*headers))
    print("-"*100)
    
    for config in configs:
        name = Path(config['file']).stem
        i2s = config['i2s_parameters']
        
        row = [
            name,
            str(i2s.get('sample_rate', 'N/A')),
            str(i2s.get('bit_depth', 16)),
            str(i2s.get('channel_count', 1)),
            f"{i2s.get('gain_db', 'N/A')}",
            str(i2s.get('is_pdm', False)),
            str(config['digital_filters'].get('decimation', 'N/A'))
        ]
        
        print(header_format.format(*row))
    
    print("="*100)


def main():
    parser = argparse.ArgumentParser(
        description='Analyze ESP32 audio configuration from source code'
    )
    parser.add_argument('--file', type=Path, help='ESP32 sketch file to analyze')
    parser.add_argument('--compare', nargs='+', type=Path,
                        help='Multiple config files to compare')
    
    args = parser.parse_args()
    
    if not args.file and not args.compare:
        parser.error('Either --file or --compare is required')
    
    if args.file:
        config = analyze_esp32_file(args.file)
        
        print(f"\nAnalysis of {args.file}:")
        print(json.dumps(config, indent=2))
        
    elif args.compare:
        configs = []
        for file_path in args.compare:
            if not file_path.exists():
                print(f"Warning: File not found: {file_path}")
                continue
            configs.append(analyze_esp32_file(file_path))
        
        generate_comparison_table(configs)


if __name__ == '__main__':
    main()
