#!/usr/bin/env python3
"""
Experiment Tracker for ESP32 Audio Testing

Tracks, compares, and visualizes WER experiments over time.
Supports saving/loading experiment history and generating comparison plots.

Usage:
    # Initialize a new experiment
    python experiment_tracker.py init
    
    # Add results from an evaluation run
    python experiment_tracker.py add --name config_v1 --results results/config_v1_results.json
    
    # List all saved experiments
    python experiment_tracker.py list
    
    # Generate comparison plot
    python experiment_tracker.py compare --output plots/comparison.png

Dependencies:
    - pandas
    - matplotlib (for plotting)
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional

try:
    import pandas as pd
    import matplotlib.pyplot as plt
    HAS_PLOTS = True
except ImportError:
    HAS_PLOTS = False


EXPERIMENTS_FILE = "experiments.json"
RESULTS_DIR = "results"


def load_experiments(base_path: Path) -> Dict:
    """Load experiments from JSON file."""
    experiments_path = base_path / EXPERIMENTS_FILE
    
    if not experiments_path.exists():
        return {
            "experiments": [],
            "metadata": {
                "created_at": datetime.now().isoformat(),
                "last_updated": datetime.now().isoformat()
            }
        }
    
    with open(experiments_path, 'r') as f:
        return json.load(f)


def save_experiments(base_path: Path, data: Dict) -> None:
    """Save experiments to JSON file."""
    data["metadata"]["last_updated"] = datetime.now().isoformat()
    with open(base_path / EXPERIMENTS_FILE, 'w') as f:
        json.dump(data, f, indent=2)


def add_experiment(
    base_path: Path,
    name: str,
    results_file: Path,
    notes: Optional[str] = None
) -> bool:
    """Add experiment results to tracker."""
    data = load_experiments(base_path)
    
    # Load results file
    if not results_file.exists():
        print(f"Error: Results file not found: {results_file}")
        return False
    
    with open(results_file, 'r') as f:
        results = json.load(f)
    
    # Extract summary metrics
    summary = results.get("summary", {})
    
    experiment = {
        "id": len(data["experiments"]),
        "name": name,
        "timestamp": datetime.now().isoformat(),
        "wer": summary.get("average_wer"),
        "accuracy_pct": summary.get("avg_accuracy_pct"),
        "utterances_tested": summary.get("total_utterances", 0),
        "rms_level": summary.get("avg_rms"),
        "clipping_pct": summary.get("avg_clipping_pct"),
        "snr_db": summary.get("avg_snr_db"),
        "speech_band_ratio": summary.get("speech_band_ratio"),
        "spectral_centroid": summary.get("spectral_centroid"),
        "notes": notes or "",
        "source_file": str(results_file)
    }
    
    data["experiments"].append(experiment)
    save_experiments(base_path, data)
    
    print(f"Added experiment: {name}")
    print(f"  WER: {experiment['wer']:.3f} (accuracy: {experiment['accuracy_pct']:.1f}%)")
    print(f"  Clipping: {experiment['clipping_pct']:.2f}%")
    print(f"  SNR: {experiment['snr_db']:.1f} dB")
    
    return True


def list_experiments(base_path: Path) -> None:
    """List all saved experiments."""
    data = load_experiments(base_path)
    
    if not data["experiments"]:
        print("No experiments recorded yet.")
        print("Run: python experiment_tracker.py add --name <config> --results <json>")
        return
    
    print("\n" + "="*90)
    print(f"{'ID':<4} {'Name':<20} {'Date':<20} {'WER':<8} {'Acc%':<7} {'SNR(dB)':<10}")
    print("-"*90)
    
    for exp in data["experiments"]:
        date_str = exp["timestamp"][:16].replace("T", " ")
        wer = exp.get("wer", float('nan'))
        acc = exp.get("accuracy_pct", 0)
        snr = exp.get("snr_db", 0)
        
        print(f"{exp['id']:<4} {exp['name']:<20} {date_str:<20} {wer:>8.3f} {acc:>7.1f} {snr:>10.1f}")
    
    print("="*90)


def generate_comparison_plot(base_path: Path, output_path: Optional[Path] = None) -> None:
    """Generate comparison plots for all experiments."""
    if not HAS_PLOTS:
        print("Error: matplotlib and pandas required. Install with:")
        print("  pip install matplotlib pandas")
        return
    
    data = load_experiments(base_path)
    
    if not data["experiments"]:
        print("No experiments to compare.")
        return
    
    df = pd.DataFrame(data["experiments"])
    
    # Create figure
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle('ESP32 Audio Configuration Comparison', fontsize=16)
    
    # WER comparison
    ax1 = axes[0, 0]
    bars = ax1.bar(df["name"], df["wer"], color='steelblue')
    ax1.set_ylabel('Word Error Rate (WER)')
    ax1.set_title('WER by Configuration')
    ax1.tick_params(axis='x', rotation=45)
    
    # Add value labels on bars
    for bar, val in zip(bars, df["wer"]):
        ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01,
                f'{val:.3f}', ha='center', va='bottom', fontsize=9)
    
    # Clipping ratio comparison
    ax2 = axes[0, 1]
    bars2 = ax2.bar(df["name"], df["clipping_pct"], color='coral')
    ax2.set_ylabel('Clipping Ratio (%)')
    ax2.set_title('Clipping Ratio by Configuration')
    ax2.tick_params(axis='x', rotation=45)
    
    for bar, val in zip(bars2, df["clipping_pct"]):
        ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.1,
                f'{val:.2f}%', ha='center', va='bottom', fontsize=9)
    
    # SNR comparison
    ax3 = axes[1, 0]
    bars3 = ax3.bar(df["name"], df["snr_db"], color='seagreen')
    ax3.set_ylabel('SNR (dB)')
    ax3.set_title('Signal-to-Noise Ratio by Configuration')
    ax3.tick_params(axis='x', rotation=45)
    
    for bar, val in zip(bars3, df["snr_db"]):
        ax3.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.1,
                f'{val:.1f}', ha='center', va='bottom', fontsize=9)
    
    # RMS level comparison
    ax4 = axes[1, 1]
    bars4 = ax4.bar(df["name"], df["rms_level"], color='mediumpurple')
    ax4.set_ylabel('RMS Level')
    ax4.set_title('RMS Level by Configuration')
    ax4.tick_params(axis='x', rotation=45)
    
    for bar, val in zip(bars4, df["rms_level"]):
        ax4.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01,
                f'{val:.3f}', ha='center', va='bottom', fontsize=9)
    
    plt.tight_layout()
    
    if output_path:
        plt.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Plot saved to: {output_path}")
    else:
        plt.show()


def get_best_config(base_path: Path) -> Optional[Dict]:
    """Return the best performing configuration based on WER."""
    data = load_experiments(base_path)
    
    if not data["experiments"]:
        return None
    
    # Sort by WER (lower is better)
    sorted_exp = sorted(data["experiments"], key=lambda x: x.get("wer", float('inf')))
    
    return sorted_exp[0]


def main():
    parser = argparse.ArgumentParser(
        description='Track and compare ESP32 audio experiments'
    )
    subparsers = parser.add_subparsers(dest="command", help="Available commands")
    
    # Add command
    add_parser = subparsers.add_parser("add", help="Add experiment results")
    add_parser.add_argument("--name", required=True, help="Configuration name")
    add_parser.add_argument("--results", type=Path, required=True,
                           help="Results JSON file from evaluate_wer.py")
    add_parser.add_argument("--notes", help="Optional notes about this configuration")
    
    # List command
    subparsers.add_parser("list", help="List all experiments")
    
    # Compare command
    compare_parser = subparsers.add_parser("compare", help="Generate comparison plot")
    compare_parser.add_argument("--output", type=Path, help="Output path for plot image")
    
    # Best command
    subparsers.add_parser("best", help="Show best performing configuration")
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return
    
    base_path = Path(__file__).parent
    
    if args.command == "add":
        add_experiment(base_path, args.name, args.results, args.notes)
    
    elif args.command == "list":
        list_experiments(base_path)
    
    elif args.command == "compare":
        generate_comparison_plot(base_path, args.output)
    
    elif args.command == "best":
        best = get_best_config(base_path)
        if best:
            print(f"\nBest configuration: {best['name']}")
            print(f"  WER: {best['wer']:.3f}")
            print(f"  Accuracy: {best['accuracy_pct']:.1f}%")
            print(f"  SNR: {best['snr_db']:.1f} dB")
        else:
            print("No experiments recorded yet.")


if __name__ == '__main__':
    main()
