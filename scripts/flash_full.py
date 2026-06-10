#!/usr/bin/env python3
"""
ESP32-S3 Modbus-MQTT Bridge — Full USB Flash via HAOS Host
Flash a brand-new or existing chip from scratch.

Flashes: bootloader → partitions → app0 → app1 → otadata → boot
Preserves: NVS (WiFi credentials, config at 0x9000)

Usage:
  python3 scripts/flash_full.py                    # flash everything
  python3 scripts/flash_full.py --firmware-only     # only app0+app1+otadata (no boot/part)

Requirements: ESP32-S3 connected via USB to HAOS host at /dev/ttyACM0
"""
import paramiko, os, sys, time, struct, argparse

# ── Configuration ──
HAOS_HOST = "192.168.1.43"
HAOS_USER = "root"
HAOS_PASS = "2009December16"
PORT = "/dev/ttyACM0"
BAUD = "460800"

BUILD_ENV = "esp32-s3"
LOCAL_BUILD = f"/config/.hermes/projects/modbus-mqtt-ha/.pio/build/{BUILD_ENV}"
REMOTE_DIR = "/share/esp32_flash"

# ── Partition offsets (must match partitions_16mb.csv) ──
ADDR_BOOTLOADER = "0x0"
ADDR_PARTITIONS  = "0x8000"
ADDR_OTADATA     = "0x10000"
ADDR_APP0        = "0x20000"    # CRITICAL: NOT 0x10000!
ADDR_APP1        = "0x320000"

FILES = {
    "bootloader":  (f"{LOCAL_BUILD}/bootloader.bin",  ADDR_BOOTLOADER),
    "partitions":  (f"{LOCAL_BUILD}/partitions.bin",   ADDR_PARTITIONS),
    "app0":        (f"{LOCAL_BUILD}/firmware.bin",      ADDR_APP0),
    "app1":        (f"{LOCAL_BUILD}/firmware.bin",       ADDR_APP1),
}

def ssh_exec(ssh, cmd, timeout=180):
    """Execute command on HAOS host, return (exit_code, stdout, stderr)."""
    stdin, stdout, stderr = ssh.exec_command(cmd, timeout=timeout)
    exit_code = stdout.channel.recv_exit_status()
    out = stdout.read().decode('utf-8', errors='replace')
    err = stderr.read().decode('utf-8', errors='replace')
    return exit_code, out, err

def flash_region(ssh, label, addr, remote_file):
    """Flash a single region via esptool."""
    cmd = (f"python3 -m esptool --chip esp32s3 --port {PORT} --baud {BAUD} "
           f"--before default-reset --after hard-reset "
           f"write-flash {addr} {remote_file} 2>&1")
    rc, out, err = ssh_exec(ssh, cmd, timeout=180)
    
    if "Hash of data verified" in out:
        size_kb = int(out.split("Wrote")[1].split("bytes")[0].strip().replace(",","")) / 1024 if "Wrote" in out else 0
        print(f"  ✅ {label} @ {addr} — {size_kb:.0f} KB verified")
        return True
    else:
        print(f"  ❌ {label} @ {addr} FAILED!")
        print(f"     {out[-300:]}")
        return False

def create_otadata(ssh):
    """Create otadata binary on remote host — boot from app0."""
    # otadata format: 2 slots of 16 bytes each (0x2000 total)
    # Slot 0: magic=0xE19D0159, seq=1 (odd = valid, boot app0)
    # Slot 1: invalid
    cmd = ('python3 -c "'
           'import struct;'
           'd=bytearray(0x2000);'
           'struct.pack_into("<IIQB",d,0,0xE19D0159,1,0xFF);'  # slot 0 CRC+seq+boot_idx
           'open(\\"/tmp/otadata_full.bin\\",\\"wb\\").write(d);'
           'print(\\"otadata_created\\")'
           '" 2>&1')
    rc, out, err = ssh_exec(ssh, cmd, timeout=10)
    return "otadata_created" in out

def main():
    parser = argparse.ArgumentParser(description="Full ESP32-S3 flash via HAOS host USB")
    parser.add_argument("--firmware-only", action="store_true",
                       help="Only flash app0+app1+otadata (skip bootloader/partitions)")
    parser.add_argument("--skip-otadata", action="store_true",
                       help="Skip otadata flash (both partitions have same fw)")
    args = parser.parse_args()

    print("=" * 60)
    print("ESP32-S3 Modbus-MQTT Bridge — Full USB Flash")
    print("=" * 60)

    # ── Verify build files exist ──
    if args.firmware_only:
        required = {"app0": FILES["app0"], "app1": FILES["app1"]}
    else:
        required = FILES

    for name, (path, addr) in required.items():
        if not os.path.exists(path):
            print(f"❌ Missing: {path}")
            print(f"   Run: pio run -e {BUILD_ENV}")
            sys.exit(1)

    fw_size = os.path.getsize(required["app0"][0])
    print(f"\nFirmware: {fw_size:,} bytes ({fw_size/1024:.1f} KB)")
    print(f"Build env: {BUILD_ENV}")
    print(f"Target: {PORT}")
    print(f"Mode: {'firmware only' if args.firmware_only else 'full flash (bootloader+partitions+fw+otadata)'}")

    # Sanity check
    assert ADDR_APP0 == "0x20000", f"CRITICAL: APP0 must be at 0x20000, got {ADDR_APP0}"

    # ── Connect SSH ──
    print(f"\nConnecting to HAOS host {HAOS_HOST}...")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HAOS_HOST, username=HAOS_USER, password=HAOS_PASS)
    print("Connected ✅")

    # ── Create remote dir + kill stale processes ──
    ssh_exec(ssh, f"mkdir -p {REMOTE_DIR}")
    ssh_exec(ssh, "pkill -f esptool 2>/dev/null; pkill -f 'python3 -m esptool' 2>/dev/null; sleep 1")

    # ── Upload files via SCP ──
    sftp = ssh.open_sftp()
    print(f"\n📤 Uploading files to {REMOTE_DIR}...")

    uploaded = set()  # avoid uploading firmware.bin twice (app0/app1 same file)
    for name, (local_path, addr) in required.items():
        if local_path in uploaded:
            continue
        remote_path = f"{REMOTE_DIR}/{os.path.basename(local_path)}"
        size = os.path.getsize(local_path)
        print(f"  {os.path.basename(local_path)}: {size:,} bytes...", end=" ", flush=True)
        sftp.put(local_path, remote_path)
        rstat = sftp.stat(remote_path)
        if rstat.st_size == size:
            print("✅")
        else:
            print(f"❌ MISMATCH! Remote={rstat.st_size}")
            sys.exit(1)
        uploaded.add(local_path)
    sftp.close()

    # ── Verify chip ──
    print("\n🔍 Verifying ESP32-S3 chip...")
    rc, out, err = ssh_exec(ssh, f"python3 -m esptool --port {PORT} --before default-reset --after hard-reset chip-id 2>&1", timeout=20)
    if "ESP32-S3" in out:
        for line in out.split('\n'):
            if any(k in line for k in ["Chip type", "Features", "MAC", "Crystal"]):
                print(f"  {line.strip()}")
    else:
        print(f"  ⚠️  Could not verify chip: {out[:200]}")
        print("  Continuing anyway...")

    # ── Flash regions ──
    print(f"\n🔥 Flashing...")
    success = True

    if not args.firmware_only:
        # Bootloader
        if not flash_region(ssh, "bootloader", ADDR_BOOTLOADER, f"{REMOTE_DIR}/bootloader.bin"):
            success = False

        # Partitions
        if success and not flash_region(ssh, "partitions", ADDR_PARTITIONS, f"{REMOTE_DIR}/partitions.bin"):
            success = False

    # App0
    if success and not flash_region(ssh, "app0", ADDR_APP0, f"{REMOTE_DIR}/firmware.bin"):
        success = False

    # App1 (same firmware, different partition)
    if success and not flash_region(ssh, "app1", ADDR_APP1, f"{REMOTE_DIR}/firmware.bin"):
        success = False

    # Otadata — boot from app0
    if success and not args.skip_otadata:
        print("\n📋 Creating otadata (boot from app0)...")
        if create_otadata(ssh):
            if not flash_region(ssh, "otadata", ADDR_OTADATA, "/tmp/otadata_full.bin"):
                print("  ⚠️  Otadata flash failed (non-critical, both partitions have same fw)")
        else:
            print("  ⚠️  Otadata creation failed (non-critical)")

    if not success:
        print("\n❌ Flash FAILED!")
        ssh.close()
        sys.exit(1)

    # ── Boot ──
    print("\n🚀 Resetting ESP32-S3...")
    rc, out, err = ssh_exec(ssh, f"python3 -m esptool --port {PORT} --before default-reset --after hard-reset run 2>&1", timeout=15)
    print("  Reset signal sent ✅")

    ssh.close()

    print(f"\n{'=' * 60}")
    print("✅ Flash complete!")
    print(f"{'=' * 60}")
    print("\n⏳ Wait ~30s for boot, then verify:")
    print("  curl -s 'http://192.168.1.67/api/status?auth=admin'")
    print("\n💡 If WiFi credentials need setup:")
    print("  curl 'http://<wifi_ip>/api/wifi?ssid=YOUR_SSID&pass=YOUR_PASS&auth=admin'")

if __name__ == "__main__":
    main()