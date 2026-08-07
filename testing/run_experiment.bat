@echo off
:: ESP32 Audio Experiment Runner (Windows)
:: Runs WER evaluation across multiple configurations

set SCRIPT_DIR=%~dp0
set BASE_DIR=%SCRIPT_DIR%..

echo === ESP32 Audio Experiment Runner ===
echo Working directory: %BASE_DIR%
echo.

REM Check Python dependencies
python -c "import faster_whisper, jiwer, numpy, soundfile" 2>nul || (
    echo Installing requirements...
    pip install -r "%SCRIPT_DIR%requirements.txt"
)

:: Default configs to test (update these with your actual config names)
set CONFIGS=config_v1 config_v2 config_v3

echo Configs to evaluate: %CONFIGS%
echo.

REM Run evaluation for each config
for %%c in (%CONFIGS%) do (
    echo --- Evaluating %%c ---
    
    set CONFIG_DIR=%BASE_DIR%\test_configs\%%c
    if exist "!CONFIG_DIR!" (
        python "%SCRIPT_DIR%evaluate_wer.py" ^
            --config-dir "!CONFIG_DIR!" ^
            --transcripts "%SCRIPT_DIR%test_corpus\gold_transcripts.txt" ^
            --output-dir "%SCRIPT_DIR%results" ^
            --model-size large-v3
        
        echo.
    ) else (
        echo Warning: Config directory not found: !CONFIG_DIR!
    )
)

:: Generate comparison summary
python -c "
import json
from pathlib import Path

results_dir = Path(r'%SCRIPT_DIR%results')
configs = list(results_dir.glob('*.json'))

print()
print('='*70)
print('EXPERIMENT SUMMARY')
print('='*70)
print(f\"{'Config':<25} {'WER':>8} {'RMS':>10} {'Clp%%':>8} {'SNR(dB)':>10}\")
print('-'*70)

for f in sorted(configs):
    with open(f) as fp:
        data = json.load(fp)
    summary = data.get('summary', {})
    wer = summary.get('average_wer', float('nan'))
    rms = summary.get('avg_rms', 0)
    clipping = summary.get('avg_clipping_pct', 0)
    snr = summary.get('avg_snr_db', 0)
    
    print(f\"{f.stem:<25} {wer:>8.3f} {rms:>10.4f} {clipping:>8.2f} {snr:>10.1f}\")

print('='*70)
"

echo.
echo Done! Results saved to: %SCRIPT_DIR%results\
