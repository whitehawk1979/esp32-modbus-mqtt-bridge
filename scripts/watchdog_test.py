#!/usr/bin/env python3
"""
watchdog_test.py — ESP32-S3 Watchdog & Crash Monitor

Monitors serial output for WDT resets, Guru Meditation errors, and crash
backtraces. On crash detection, attempts symbolication using the firmware ELF
file and xtensa-objdump from the PlatformIO toolchain.

Usage:
    python3 scripts/watchdog_test.py --port /dev/ttyUSB0 --baud 115200 --duration 60

Optional arguments:
    --elf       Path to firmware.elf (default: auto-detect from .pio/build)
    --toolchain Path to xtensa toolchain dir (default: auto-detect)

Requires only Python stdlib. Uses subprocess for esptool and xtensa-objdump.
"""

import argparse
import os
import re
import serial
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

# ─── Project paths ─────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", "esp32-s3-prod")
DEFAULT_ELF = os.path.join(BUILD_DIR, "firmware.elf")
PIO_PACKAGES = os.path.join(PROJECT_DIR, ".pio", "packages")

# ─── Crash / WDT patterns ──────────────────────────────────────────
PATTERNS = {
    "wdt_reset": [
        re.compile(r"rst:(0x\w+)", re.IGNORECASE),
        re.compile(r"WDT", re.IGNORECASE),
        re.compile(r"watchdog", re.IGNORECASE),
        re.compile(r"Task WDT", re.IGNORECASE),
        re.compile(r"Interrupt WDT", re.IGNORECASE),
    ],
    "crash": [
        re.compile(r"Guru Meditation Error", re.IGNORECASE),
        re.compile(r"core 0 register dump", re.IGNORECASE),
        re.compile(r"core 1 register dump", re.IGNORECASE),
        re.compile(r"panic", re.IGNORECASE),
        re.compile(r"Backtrace:", re.IGNORECASE),
        re.compile(r"backtrace:", re.IGNORECASE),
    ],
}

BACKTRACE_RE = re.compile(r"[Bb]acktrace:\s+(0x[0-9a-fA-F]{8}(?:\s+0x[0-9a-fA-F]{8})*)")
PC_RE = re.compile(r"PC\s+(?:at|:)\s+(0x[0-9a-fA-F]{8})")


def find_xtensa_objdump():
    """Find xtensa-esp-elf-objdump in PlatformIO toolchain packages."""
    # Check standard PlatformIO toolchain location
    toolchain_dir = os.path.join(PIO_PACKAGES, "toolchain-xtensa-esp-elf")
    if os.path.isdir(toolchain_dir):
        objdump = os.path.join(toolchain_dir, "bin", "xtensa-esp-elf-objdump")
        if os.path.isfile(objdump):
            return objdump

    # Search broader
    for root, dirs, files in os.walk(PIO_PACKAGES):
        for f in files:
            if "xtensa" in f and "objdump" in f:
                return os.path.join(root, f)

    # Fallback: check PATH
    try:
        result = subprocess.run(
            ["which", "xtensa-esp-elf-objdump"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except Exception:
        pass

    return None


def find_esptool():
    """Find esptool.py from PlatformIO packages."""
    esptool_path = os.path.join(PIO_PACKAGES, "tool-esptoolpy", "esptool.py")
    if os.path.isfile(esptool_path):
        return esptool_path

    # Check PATH
    try:
        result = subprocess.run(
            ["which", "esptool.py"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except Exception:
        pass

    return None


def decode_backtrace_with_objdump(backtrace_addrs, elf_path, objdump_path):
    """Attempt to symbolicate backtrace addresses using xtensa-objdump."""
    if not objdump_path or not elf_path or not os.path.isfile(elf_path):
        return None

    results = []
    for addr in backtrace_addrs:
        try:
            # Use objdump --start-address to find the function containing this address
            result = subprocess.run(
                [objdump_path, "-d", "--start-address=" + addr,
                 "--stop-address=" + hex(int(addr, 16) + 4),
                 elf_path],
                capture_output=True, text=True, timeout=10
            )
            output = result.stdout.strip()
            if output:
                results.append(f"  {addr}: {output.split(chr(10))[-1].strip()}")
            else:
                results.append(f"  {addr}: (no symbol found)")
        except Exception as e:
            results.append(f"  {addr}: (decode error: {e})")

    return "\n".join(results) if results else None


def decode_backtrace_with_addr2line(backtrace_addrs, elf_path):
    """Attempt symbolication using addr2line if available."""
    # xtensa-esp-elf-addr2line
    addr2line_path = None
    toolchain_dir = os.path.join(PIO_PACKAGES, "toolchain-xtensa-esp-elf")
    if os.path.isdir(toolchain_dir):
        candidate = os.path.join(toolchain_dir, "bin", "xtensa-esp-elf-addr2line")
        if os.path.isfile(candidate):
            addr2line_path = candidate

    if not addr2line_path:
        try:
            result = subprocess.run(
                ["which", "xtensa-esp-elf-addr2line"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0 and result.stdout.strip():
                addr2line_path = result.stdout.strip()
        except Exception:
            pass

    if not addr2line_path or not elf_path or not os.path.isfile(elf_path):
        return None

    results = []
    for addr in backtrace_addrs:
        try:
            result = subprocess.run(
                [addr2line_path, "-f", "-p", "-e", elf_path, addr],
                capture_output=True, text=True, timeout=10
            )
            output = result.stdout.strip()
            results.append(f"  {addr}: {output}" if output else f"  {addr}: (no symbol)")
        except Exception as e:
            results.append(f"  {addr}: (addr2line error: {e})")

    return "\n".join(results) if results else None


def read_coredump(esptool_path, port, baud):
    """Attempt to read the coredump partition via esptool.
    ESP32-S3 stores coredumps in a dedicated flash partition."""

    if not esptool_path:
        print("[WARN] esptool.py not found — cannot read coredump partition")
        return None

    # Common coredump partition names
    partition_names = ["coredump", "core0", "core1"]

    for part_name in partition_names:
        try:
            # Read partition using esptool read_flash (requires knowing offset/size)
            # We'll use the partition table to find the coredump partition
            result = subprocess.run(
                [sys.executable or "python3", esptool_path,
                 "--chip", "esp32s3", "--port", port, "--baud", str(baud),
                 "partition_table"],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode == 0 and part_name in result.stdout.lower():
                print(f"[INFO] Found '{part_name}' partition in partition table")
                # Parse offset and size from partition table output
                for line in result.stdout.split("\n"):
                    if part_name in line.lower():
                        print(f"  Partition line: {line.strip()}")
                break
        except subprocess.TimeoutExpired:
            print(f"[WARN] esptool partition_table read timed out")
            break
        except Exception as e:
            print(f"[WARN] esptool partition read failed: {e}")
            break

    return None


class CrashMonitor:
    """Monitors serial output for WDT resets and crashes."""

    def __init__(self, port, baud, duration, elf_path=None):
        self.port = port
        self.baud = baud
        self.duration = duration
        self.elf_path = elf_path or DEFAULT_ELF

        self.wdt_reset_count = 0
        self.crash_count = 0
        self.backtraces = []
        self.log_lines = []
        self.collecting_backtrace = False
        self.current_backtrace_buf = []
        self.running = True

        self.objdump_path = find_xtensa_objdump()
        self.esptool_path = find_esptool()

    def _timestamp(self):
        return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

    def _check_line(self, line):
        """Check a serial line against known crash/WDT patterns."""
        is_wdt = False
        is_crash = False

        for name, patterns in PATTERNS.items():
            for pat in patterns:
                if pat.search(line):
                    if name == "wdt_reset":
                        is_wdt = True
                    elif name == "crash":
                        is_crash = True

        if is_wdt:
            self.wdt_reset_count += 1

        if is_crash:
            self.crash_count += 1
            self.collecting_backtrace = True
            self.current_backtrace_buf = [line]

        # Collect backtrace continuation lines
        if self.collecting_backtrace and not is_crash:
            self.current_backtrace_buf.append(line)
            # Backtrace section typically ends with a blank line or
            # a "Rebooting" message
            if not line.strip() or "reboot" in line.lower():
                self._finalize_backtrace()

    def _finalize_backtrace(self):
        """Process collected backtrace lines and attempt symbolication."""
        self.collecting_backtrace = False
        buf = self.current_backtrace_buf
        self.current_backtrace_buf = []

        if not buf:
            return

        raw_text = "\n".join(buf)

        # Extract addresses from backtrace line
        backtrace_addrs = []
        match = BACKTRACE_RE.search(raw_text)
        if match:
            backtrace_addrs = match.group(1).split()

        # Also check for PC address
        pc_match = PC_RE.search(raw_text)
        if pc_match and pc_match.group(1) not in backtrace_addrs:
            backtrace_addrs.insert(0, pc_match.group(1))

        # Attempt symbolication
        decoded = None
        if backtrace_addrs:
            decoded = decode_backtrace_with_addr2line(backtrace_addrs, self.elf_path)
            if not decoded:
                decoded = decode_backtrace_with_objdump(
                    backtrace_addrs, self.elf_path, self.objdump_path
                )

        entry = {
            "raw": raw_text,
            "addresses": backtrace_addrs,
            "decoded": decoded,
        }
        self.backtraces.append(entry)

    def run(self):
        """Open serial port and monitor for the specified duration."""
        print(f"[INFO] ESP32-S3 Watchdog & Crash Monitor")
        print(f"[INFO] Port: {self.port} @ {self.baud} baud")
        print(f"[INFO] Duration: {self.duration}s")
        print(f"[INFO] ELF: {self.elf_path} {'(found)' if os.path.isfile(self.elf_path) else '(NOT FOUND)'}")
        print(f"[INFO] xtensa-objdump: {self.objdump_path or '(NOT FOUND)'}")
        print(f"[INFO] esptool: {self.esptool_path or '(NOT FOUND)'}")
        print(f"[INFO] Monitoring... (Ctrl+C to stop early)")
        print()

        # Attempt to read coredump if device is connected
        if self.esptool_path:
            print("[INFO] Checking for coredump partition...")
            read_coredump(self.esptool_path, self.port, self.baud)
            print()

        start_time = time.time()
        try:
            ser = serial.Serial(self.port, self.baud, timeout=1)
        except serial.SerialException as e:
            print(f"[ERROR] Cannot open serial port {self.port}: {e}")
            sys.exit(1)

        # Handle Ctrl-C gracefully
        def signal_handler(sig, frame):
            print("\n[INFO] Interrupted — stopping monitor...")
            self.running = False

        signal.signal(signal.SIGINT, signal_handler)

        try:
            while self.running:
                elapsed = time.time() - start_time
                if self.duration > 0 and elapsed >= self.duration:
                    break

                try:
                    line = ser.readline()
                except serial.SerialException:
                    break

                if not line:
                    continue

                # Decode — ESP32 sends UTF-8
                try:
                    text = line.decode("utf-8", errors="replace").rstrip("\r\n")
                except Exception:
                    text = line.decode("latin-1", errors="replace").rstrip("\r\n")

                if not text:
                    # Blank line may end a backtrace section
                    if self.collecting_backtrace:
                        self._finalize_backtrace()
                    continue

                ts = self._timestamp()
                self.log_lines.append((ts, text))
                print(f"[{ts}] {text}")

                self._check_line(text)

        finally:
            ser.close()

        # Finalize any pending backtrace
        if self.collecting_backtrace:
            self._finalize_backtrace()

        self._print_summary()

    def _print_summary(self):
        """Print monitoring summary with crash statistics and decoded backtraces."""
        print()
        print("=" * 64)
        print("  WATCHDOG TEST SUMMARY")
        print("=" * 64)
        print(f"  Total lines logged:  {len(self.log_lines)}")
        print(f"  WDT resets detected: {self.wdt_reset_count}")
        print(f"  Crashes detected:     {self.crash_count}")
        print()

        if self.backtraces:
            print(f"  Decoded backtraces ({len(self.backtraces)}):")
            print("-" * 64)
            for i, bt in enumerate(self.backtraces, 1):
                print(f"\n  Crash #{i}:")
                # Print key lines from raw backtrace (skip register dumps for brevity)
                for line in bt["raw"].split("\n"):
                    line = line.strip()
                    if not line:
                        continue
                    if any(kw in line.lower() for kw in
                           ["backtrace", "guru meditation", "pc ", "panic"]):
                        print(f"    {line}")
                if bt["addresses"]:
                    print(f"    Addresses: {' '.join(bt['addresses'])}")
                if bt["decoded"]:
                    print(f"    Decoded:")
                    print(bt["decoded"])
                elif bt["addresses"]:
                    print(f"    (Symbolication failed — ELF or objdump not available)")
                print()
        else:
            print("  No crashes or backtraces detected. ✓")

        print("=" * 64)

        # Return exit code: 0 = no crashes, 1 = crashes found
        return 1 if self.crash_count > 0 else 0


def main():
    parser = argparse.ArgumentParser(
        description="ESP32-S3 Watchdog & Crash Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 scripts/watchdog_test.py --port /dev/ttyUSB0 --baud 115200 --duration 60
  python3 scripts/watchdog_test.py --port /dev/ttyACM0 --duration 0  # run forever
  python3 scripts/watchdog_test.py --port COM3 --elf ./custom.elf
        """,
    )
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyUSB0, COM3)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--duration", type=int, default=60,
                        help="Monitoring duration in seconds (0 = forever, default: 60)")
    parser.add_argument("--elf", default=DEFAULT_ELF,
                        help=f"Path to firmware.elf (default: {DEFAULT_ELF})")
    parser.add_argument("--log", default=None,
                        help="Save all serial output to this file")

    args = parser.parse_args()

    monitor = CrashMonitor(
        port=args.port,
        baud=args.baud,
        duration=args.duration,
        elf_path=args.elf,
    )

    exit_code = monitor.run()

    # Optionally save full log
    if args.log and monitor.log_lines:
        with open(args.log, "w") as f:
            for ts, line in monitor.log_lines:
                f.write(f"[{ts}] {line}\n")
        print(f"[INFO] Full log saved to {args.log}")

    sys.exit(exit_code if isinstance(exit_code, int) else 0)


if __name__ == "__main__":
    main()