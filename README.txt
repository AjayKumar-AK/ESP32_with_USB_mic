ESP32-S3 USB microphone -> LittleFS -> BLE -> Windows PC

Flow:
1. ESP32 boots.
2. Motor runs for 1 second.
3. ESP32 waits for a USB UAC microphone for 4 seconds.
4. If a microphone is detected, it records 10 seconds.
5. The recording is saved as /littlefs/rec_001.wav.
6. The file remains in LittleFS across reboot.
7. ESP32 advertises as YAGEN_MIC.
8. Windows Python program connects over BLE.
9. Python requests GET_FILE.
10. ESP32 sends the WAV through BLE notifications.
11. Python saves rec_001.wav.

Motor status:
1 pulse  = recording started
2 pulses = recording saved
3 pulses = BLE transfer completed
4 pulses = USB/UAC initialization failed
5 pulses = error

Install Python dependency:
    py -m pip install -r requirements.txt

Run:
    py ble_receiver.py

Important:
- Do NOT erase flash between recording and BLE transfer.
- Do NOT change LittleFS to format_if_mount_failed=true.
- Windows must have a working Bluetooth adapter.
- The USB microphone still needs proper 5 V USB-host power.
