#!/usr/bin/env python3
r"""s3_reset.py - reboot an ESP32-S3 the same way `idf.py monitor` does.

On attach, esp-idf-monitor does not talk to the ROM. It pulses the serial
RTS line, which the ESP32-S3 USB-Serial-JTAG peripheral (and a UART-bridge
auto-reset circuit) wires to chip enable. That is the reboot you see when
monitor starts, and the same one Ctrl+T Ctrl+R sends.

This is not `s3_recover.py`. That script syncs with the ROM download loader
and arms the RTC watchdog, which is the escape hatch when the chip is
latched in download mode. A normal hung app still enumerates as a COM port
and only needs this pulse. Entering the ROM loader on the Waveshare 1.8
can latch download mode until a watchdog or power-on reset.

Usage (PowerShell, IDF 5.5 venv already has pyserial):
    & "C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe" python\s3_reset.py COM4

With no port argument, the script lists the available COM ports.
"""
import sys
import time

import serial
from serial.tools import list_ports

# esp_idf_monitor.base.constants: LOW = True, HIGH = False.
# setRTS(True) holds EN low (chip in reset); setRTS(False) releases it.
# Hold matches MINIMAL_EN_LOW_DELAY in esp_idf_monitor (the monitor hard reset).
EN_LOW_HOLD_S = 0.005


def list_port_lines():
    return [f"{p.device}: {p.description} [{p.vid:04X}:{p.pid:04X}]"
            if p.vid is not None else f"{p.device}: {p.description}"
            for p in list_ports.comports()]


def _set_rts(ser, value):
    """Set RTS, then rewrite DTR so Windows usbser.sys actually sends it.

    usbser.sys only emits SET_CONTROL_LINE_STATE when DTR changes. Monitor
    and esptool both follow every setRTS with setDTR(current).
    """
    ser.setRTS(value)
    ser.setDTR(ser.dtr)


def hard_reset(port_name):
    """Copy of esp_idf_monitor SerialReader.open_serial(reset=True) + Reset.hard."""
    ser = serial.Serial()
    ser.port = port_name
    ser.baudrate = 115200
    ser.timeout = 0.1
    # Monitor asserts both lines before open, so the port comes up in reset
    # rather than glitching GPIO0 (DTR) on the way out.
    ser.dtr = True
    ser.rts = True
    ser.open()
    try:
        # Release EN before DTR so the strap is not sampled as download mode.
        _set_rts(ser, False)
        ser.setDTR(False)
        time.sleep(0.05)
        _set_rts(ser, True)  # EN low
        time.sleep(EN_LOW_HOLD_S)
        _set_rts(ser, False)  # EN high, chip boots the app
        time.sleep(0.05)
    finally:
        try:
            _set_rts(ser, False)
            ser.setDTR(False)
        except (serial.SerialException, OSError):
            # USB-Serial-JTAG drops off the bus as the chip resets. The
            # pulse already went out; losing the handle is the reboot.
            pass
        ser.close()


def main():
    if len(sys.argv) < 2:
        print("Usage: python s3_reset.py <COM port>")
        ports = list_port_lines()
        if ports:
            print("\nAvailable ports:")
            for line in ports:
                print(f"  {line}")
        else:
            print("\nNo serial ports found - is the board plugged in?")
        return 2

    port = sys.argv[1]
    try:
        hard_reset(port)
    except serial.SerialException as exc:
        print(f"Could not open {port}: {exc}")
        print("Close idf.py monitor first if it still has the port.")
        return 1
    print(f"Reset pulse sent to {port}. The app should boot from flash.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
