import argparse
import asyncio
from pathlib import Path
import struct
import sys

from bleak import BleakClient, BleakScanner

# Supported target device names (current firmware uses MIC-RECORDER; legacy used YAGEN_MIC)
KNOWN_DEVICE_NAMES = ["MIC-RECORDER", "YAGEN_MIC"]

# Primary UUIDs (firmware main.c)
SERVICE_UUID = "00ffeedd-ccbb-aa99-8877-665544332211"
CONTROL_UUID = "01ffeedd-ccbb-aa99-8877-665544332211"
DATA_UUID = "02ffeedd-ccbb-aa99-8877-665544332211"
LOG_UUID = "03ffeedd-ccbb-aa99-8877-665544332211"

# Legacy UUIDs (for backward compatibility)
LEGACY_SERVICE_UUID = "0000FFF0-0000-1000-8000-00805F9B34FB"
LEGACY_COMMAND_UUID = "0000FFF1-0000-1000-8000-00805F9B34FB"
LEGACY_DATA_UUID = "0000FFF2-0000-1000-8000-00805F9B34FB"

DEFAULT_OUTPUT_FILE = Path("recording.wav")


def print_progress(received: int, total: int, width: int = 40):
    if total <= 0:
        return
    pct = min(100.0, (received / total) * 100.0)
    filled = int(width * received // total)
    bar = "=" * filled + "-" * (width - filled)
    sys.stdout.write(f"\r[{bar}] {received:,}/{total:,} bytes ({pct:5.1f}%)")
    sys.stdout.flush()


async def main():
    parser = argparse.ArgumentParser(
        description="BLE Audio Receiver for ESP32-S3 USB Mic Recorder"
    )
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        default=DEFAULT_OUTPUT_FILE,
        help="Path where output WAV will be saved (default: recording.wav)",
    )
    parser.add_argument(
        "--name",
        "-n",
        type=str,
        default=None,
        help="BLE device name to filter for (default: MIC-RECORDER or YAGEN_MIC)",
    )
    parser.add_argument(
        "--timeout",
        "-t",
        type=float,
        default=25.0,
        help="BLE scan timeout in seconds (default: 25.0)",
    )
    parser.add_argument(
        "--record-now",
        action="store_true",
        help="Send record command (0x02) to trigger a fresh 5s recording on the ESP32",
    )
    args = parser.parse_args()

    print("==================================================")
    print(" ESP32-S3 USB Microphone BLE Receiver")
    print("==================================================")

    # 1. Scan for matching device
    target_names = [args.name] if args.name else KNOWN_DEVICE_NAMES
    print(f"Scanning for devices ({', '.join(target_names)})...")

    device = None
    discovered = await BleakScanner.discover(timeout=args.timeout)
    for d in discovered:
        d_name = d.name or ""
        if any(name.lower() in d_name.lower() for name in target_names):
            device = d
            break

    if device is None:
        print(f"\nERROR: No matching device found within {args.timeout}s.")
        print("Tips:")
        print("  - Ensure ESP32-S3 is powered on and Bluetooth is enabled on PC.")
        print("  - Wait 10s after ESP32 boot for auto-recording to complete.")
        print("  - Check UART logs or motor vibration pulses for status.")
        return 1

    print(f"Found target device: {device.name} [{device.address}]")
    print(f"Connecting to {device.address}...")

    received = bytearray()
    expected_size = None
    done_event = asyncio.Event()

    def data_handler(sender, data: bytearray):
        nonlocal expected_size

        # Handle size header packet
        if expected_size is None:
            # Current firmware: 4-byte little-endian uint32
            if len(data) == 4:
                expected_size = struct.unpack("<I", data)[0]
                print(f"\n[DATA] Expected WAV file size: {expected_size:,} bytes")
                return
            # Legacy ASCII header: SIZE=<bytes>
            try:
                text = bytes(data).decode("ascii", errors="ignore").strip()
                if text.startswith("SIZE="):
                    expected_size = int(text[5:])
                    print(f"\n[DATA] Expected WAV file size (legacy): {expected_size:,} bytes")
                    return
            except (ValueError, UnicodeDecodeError):
                pass

            print(f"\n[WARN] Unexpected size packet ({len(data)} bytes): {bytes(data).hex()}")
            return

        # Subsequent packets: audio stream payload
        remaining = expected_size - len(received)
        if remaining > 0:
            received.extend(data[:remaining])

        print_progress(len(received), expected_size)

        if len(received) >= expected_size:
            done_event.set()

    def log_handler(sender, data: bytearray):
        try:
            msg = bytes(data).decode("utf-8", errors="replace").strip()
            print(f"\n[ESP32 LOG] {msg}")
            if "XFER_COMPLETE status=OK" in msg:
                done_event.set()
        except Exception:
            pass

    async with BleakClient(device) as client:
        print(f"Connected to {device.name}!")

        # Determine which UUID set to use
        services = client.services
        has_primary_data = any(DATA_UUID.lower() in s.uuid.lower() for s in services.characteristics.values())
        has_legacy_data = any(LEGACY_DATA_UUID.lower() in s.uuid.lower() for s in services.characteristics.values())

        active_data_uuid = DATA_UUID if has_primary_data else (LEGACY_DATA_UUID if has_legacy_data else None)
        active_ctrl_uuid = CONTROL_UUID if has_primary_data else (LEGACY_COMMAND_UUID if has_legacy_data else None)
        active_log_uuid = LOG_UUID if has_primary_data else None

        if not active_data_uuid:
            print("ERROR: Audio data GATT characteristic not found on device!")
            return 1

        # Subscribe to LOG characteristic if available
        if active_log_uuid and any(active_log_uuid.lower() in s.uuid.lower() for s in services.characteristics.values()):
            try:
                await client.start_notify(active_log_uuid, log_handler)
                print(f"Subscribed to status log notifications ({active_log_uuid})")
            except Exception as e:
                print(f"[NOTE] Could not subscribe to log characteristic: {e}")

        # Subscribe to DATA indications/notifications
        print(f"Subscribed to audio data indications ({active_data_uuid})")
        await client.start_notify(active_data_uuid, data_handler)

        if args.record_now:
            if active_ctrl_uuid:
                print("Sending 0x02 to request fresh 5s recording...")
                await client.write_gatt_char(active_ctrl_uuid, b"\x02", response=True)
                print("Recording started. Waiting 7 seconds before requesting upload...")
                await asyncio.sleep(7.0)
            else:
                print("[WARN] Control characteristic not found to trigger record.")

        # Trigger upload via CONTROL characteristic
        if active_ctrl_uuid:
            print("Requesting WAV upload...")
            payload = b"\x01" if active_ctrl_uuid == CONTROL_UUID else b"GET_FILE"
            try:
                await client.write_gatt_char(active_ctrl_uuid, payload, response=True)
            except Exception as e:
                print(f"[NOTE] Control write ({payload!r}): {e}")

        print("Waiting for audio transfer to complete...")
        try:
            await asyncio.wait_for(done_event.wait(), timeout=90.0)
        except asyncio.TimeoutError:
            print("\nERROR: Audio transfer timed out after 90 seconds.")
            await client.stop_notify(active_data_uuid)
            return 1

        await client.stop_notify(active_data_uuid)
        if active_log_uuid:
            try:
                await client.stop_notify(active_log_uuid)
            except Exception:
                pass

    print("\n")
    if expected_size is None or len(received) == 0:
        print("ERROR: No audio data received.")
        return 1

    if len(received) < expected_size:
        print(f"WARNING: Received {len(received):,} of {expected_size:,} bytes (incomplete).")

    # Save output file
    output_path = args.output
    output_path.write_bytes(received)
    print("==================================================")
    print(" SUCCESS: Audio file received successfully!")
    print(f" Saved to: {output_path.resolve()}")
    print(f" Size:     {len(received):,} bytes")

    # Also keep rec_001.wav updated if saving as default
    if output_path != Path("rec_001.wav"):
        try:
            Path("rec_001.wav").write_bytes(received)
            print(" Also saved backup as: rec_001.wav")
        except Exception:
            pass
    print("==================================================")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
