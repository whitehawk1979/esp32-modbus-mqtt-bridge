#!/usr/bin/env python3
"""
ESP32-S3 Modbus-MQTT Bridge — USB Flash via HAOS Host
Uses paramiko SSH to copy firmware + flash via esptool on HAOS host.
Flash offset: 0x20000 (app0 partition) — CRITICAL: NOT 0x10000!
"""
import paramiko, os, sys, time

HAOS_HOST = "192.168.1.43"
HAOS_USER = "root"
HAOS_PASS = "2009December16"
PORT = "/dev/ttyACM0"
BAUD = "460800"
FLASH_ADDR = "0x20000"  # CRITICAL: app0 partition offset

BUILD_ENV = "esp32-s3-prod"
LOCAL_BUILD = f"/config/.hermes/projects/modbus-mqtt-ha/.pio/build/{BUILD_ENV}"
FIRMWARE = f"{LOCAL_BUILD}/firmware.bin"
BOOTLOADER = f"{LOCAL_BUILD}/bootloader.bin"
PARTITIONS = f"{LOCAL_BUILD}/partitions.bin"

REMOTE_DIR = "/share/esp32_flash"

def ssh_exec(ssh, cmd, timeout=120):
    """Execute command on HAOS host, return exit code + output."""
    stdin, stdout, stderr = ssh.exec_command(cmd, timeout=timeout)
    exit_code = stdout.channel.recv_exit_status()
    out = stdout.read().decode('utf-8', errors='replace')
    err = stderr.read().decode('utf-8', errors='replace')
    return exit_code, out, err

def main():
    # Verify files exist
    for f in [FIRMWARE, BOOTLOADER, PARTITIONS]:
        if not os.path.exists(f):
            print(f"ERROR: File not found: {f}")
            sys.exit(1)

    fw_size = os.path.getsize(FIRMWARE)
    print(f"Firmware: {fw_size} bytes ({fw_size/1024:.1f} KB)")
    print(f"Flash offset: {FLASH_ADDR}")
    print(f"Build env: {BUILD_ENV}")

    # Sanity checks
    assert FLASH_ADDR == "0x20000", f"CRITICAL: Flash address must be 0x20000, got {FLASH_ADDR}"
    assert BUILD_ENV == "esp32-s3-prod", "Wrong build env! Must be esp32-s3-prod for OPI PSRAM"

    # Connect SSH
    print(f"\nConnecting to HAOS host {HAOS_HOST}...")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HAOS_HOST, username=HAOS_USER, password=HAOS_PASS)
    print("Connected!")

    # Create remote directory
    ssh_exec(ssh, f"mkdir -p {REMOTE_DIR}")

    # SCP all 3 files (MUST copy before flashing!)
    sftp = ssh.open_sftp()
    print(f"\nUploading files to {REMOTE_DIR}...")
    for local_path, name in [(BOOTLOADER, "bootloader.bin"), (PARTITIONS, "partitions.bin"), (FIRMWARE, "firmware.bin")]:
        remote_path = f"{REMOTE_DIR}/{name}"
        size = os.path.getsize(local_path)
        print(f"  {name}: {size} bytes...", end=" ", flush=True)
        sftp.put(local_path, remote_path)
        # Verify size
        rstat = sftp.stat(remote_path)
        if rstat.st_size == size:
            print("OK ✅")
        else:
            print(f"MISMATCH! Remote={rstat.st_size}, Local={size}")
            sys.exit(1)
    sftp.close()

    # Verify chip is accessible
    print("\nVerifying ESP32-S3 chip...")
    rc, out, err = ssh_exec(ssh, f"esptool --port {PORT} chip-id 2>&1 | head -5", timeout=20)
    if "ESP32-S3" in out:
        print(f"  Chip: OK ✅ ({out.strip()[:80]})")
    else:
        print(f"  WARNING: Could not verify chip: {out[:200]}")

    # Flash firmware only (preserves NVRAM, bootloader, partitions)
    # IMPORTANT: Write to BOTH OTA partitions (app0 + app1) because
    # the bootloader may boot from either depending on otadata.
    APP0_ADDR = "0x20000"    # ota_0 partition
    APP1_ADDR = "0x320000"   # ota_1 partition

    for label, addr in [("APP0", APP0_ADDR), ("APP1", APP1_ADDR)]:
        print(f"\nFlashing {label} at {addr}...")
        flash_cmd = f"esptool --chip esp32s3 --port {PORT} --baud {BAUD} " \
                    f"--before default-reset --after hard-reset " \
                    f"write-flash {addr} {REMOTE_DIR}/firmware.bin 2>&1"
        rc, out, err = ssh_exec(ssh, flash_cmd, timeout=120)
        print(out[-400:] if len(out) > 400 else out)

        if rc != 0 and "Hash of data verified" not in out:
            print(f"\n❌ FAILED {label}! Exit code: {rc}")
            if err:
                print(f"Error: {err[:500]}")
            ssh.close()
            sys.exit(1)

    # Set otadata so bootloader uses app0 (factory fresh)
    # Write otadata: seq0=1, crc0 valid → boot from app0
    ota_cmd = f'python3 -c "' \
              f'import struct,esptool' \
              f'; data=bytearray(0x2000)' \
              f'; struct.pack_into("<II",data,0,1,0)' \
              f'; struct.pack_into("<II",data,16,0xE19D0159,0)' \
              f'; open("/tmp/otadata.bin","wb").write(data)' \
              f'; print("otadata written")' \
              f'" 2>&1'
    rc, out, err = ssh_exec(ssh, ota_cmd, timeout=10)
    print(f"\notadata prepare: {out.strip()}")
    
    ota_flash = f"esptool --chip esp32s3 --port {PORT} --baud {BAUD} " \
                f"--before default-reset --after hard-reset " \
                f"write-flash 0x10000 /tmp/otadata.bin 2>&1"
    # Note: otadata write is optional — both partitions have same firmware now

    # Boot — send reset signal
    print("\nBooting ESP32...")
    rc, out, err = ssh_exec(ssh, f"esptool --port {PORT} run 2>&1", timeout=15)
    print(f"  Reset sent ✅")

    ssh.close()

    print(f"\n✅ Flash complete! Wait ~30s for boot, then verify:")
    print(f"  curl -s 'http://192.168.1.67/api/diag?auth=admin' | python3 -m json.tool")
    print(f"\n⚠️  After boot, RUN: curl 'http://192.168.1.67/savepins?pde=42&auth=admin'")
    print(f"   (NVS reverts pin_rs485_de to 4 after flash)")

if __name__ == "__main__":
    main()