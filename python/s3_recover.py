#!/usr/bin/env python3
r"""s3_recover.py - escape an ESP32-S3 stuck in ROM download mode.

When an ESP32-S3 board with USB-Serial-JTAG (Waveshare AMOLED 1.8) or a
USB-UART bridge (Waveshare AMOLED 2.06) is stuck in download mode
("boot:0x23 (DOWNLOAD(USB/UART0))" on reset, silent monitor, app never runs),
a plain reset does not help: the strapping pins are only re-sampled on a
power-on / RTC-watchdog / brownout reset, and a USB-initiated reset cannot
re-latch them (espressif/esp-idf#13287, espressif/esptool#970).

This script connects to the ROM download-mode loader and asks esptool to
perform a "watchdog reset": it arms the RTC watchdog (which counts as a
real chip reset and re-samples the straps), clears the FORCE_DOWNLOAD_BOOT
latch in RTC_CNTL_OPTION1_REG, and lets the chip boot from flash.

Requires esptool >= 4.7 (the `--after watchdog-reset` option); the IDF 5.5
bundled version (4.12) has it. Note the value spelling changed between
releases: installed esptool 4.12 uses `watchdog_reset` (underscore), newer
master uses `watchdog-reset` (hyphen) - this script tries both.

Usage (PowerShell on this machine):
    & "C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe" python\s3_recover.py COM4

With no port argument, the script lists the available COM ports.
"""
import sys

import esptool


def list_ports():
    """Return a list of COM port descriptions (requires pyserial, present with esptool)."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    return [f"{p.device}: {p.description}" for p in list_ports.comports()]


def main():
    print("parameters: ", sys.argv, "len: ", len(sys.argv))
    if len(sys.argv) < 2:
        ports = list_ports()
        print("Usage: python s3_recover.py <COM port>")
        if ports:
            print("\nAvailable ports:")
            for p in ports:
                print(f"  {p}")
        else:
            print("\nNo serial ports found - is the board plugged in?")
        sys.exit(2)

    port = sys.argv[1]
    # `chip_id` connects to the download-mode ROM loader (verifying the link),
    # then the watchdog-reset performs the full chip reset that re-samples the
    # strapping pins so the app boots from flash.
    # esptool 4.12 spells the option `watchdog_reset`, newer releases spell it
    # `watchdog-reset`; try the installed spelling first, fall back on a usage
    # error (SystemExit code 2 from argparse).
    for after in ("watchdog_reset", "watchdog-reset"):
        try:
            rc = esptool.main(
                ["--chip", "esp32s3", "--port", port, "--baud", "115200",
                 "--after", after, "chip_id"]
            )
            break
        except SystemExit as exc:
            if exc.code == 2 and after == "watchdog_reset":
                continue  # invalid choice -> retry with the other spelling
            raise
    if rc == 0:
        print(f"\nRecovery reset issued on {port} - the app should now boot from flash.")
        print("Verify with: idf.py -p {0} monitor".format(port))
    return rc


if __name__ == "__main__":
    sys.exit(main())
