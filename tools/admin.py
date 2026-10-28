#!/usr/bin/env python3
"""Admin tool for the teacher recorder.

1. On the device: hold the button for 2 seconds (the screen shows the Bluetooth name).
2. On the PC:     pip install bleak     then     python admin.py

Sets Wi-Fi, class/section and the clock, and manages the teacher list.
Cards tapped on the device while it is in admin mode are sent to this script.
"""
import argparse
import asyncio
import csv
import getpass
import json
import time

from bleak import BleakClient, BleakScanner

CMD_UUID = "c0de0002-7e51-4a5e-9d10-7ea0c4a2b3f1"
RESP_UUID = "c0de0003-7e51-4a5e-9d10-7ea0c4a2b3f1"
NAME_PREFIX = "TCHR-REC-"


class Admin:
    def __init__(self, client):
        self.c = client
        self.buf = b""
        self.replies = asyncio.Queue()
        self.taps = asyncio.Queue()

    def on_notify(self, _sender, data):
        self.buf += bytes(data)
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                print("  (unreadable message from device:", line[:60], ")")
                continue
            if "tap" in msg:
                self.taps.put_nowait(msg["tap"])
            else:
                self.replies.put_nowait(msg)

    async def call(self, cmd, timeout=10, **kw):
        """Send a command, return (final_reply, intermediate_messages)."""
        while not self.replies.empty():
            self.replies.get_nowait()
        payload = json.dumps({"cmd": cmd, **kw}, separators=(",", ":")) + "\n"
        await self.c.write_gatt_char(CMD_UUID, payload.encode(), response=True)
        items = []
        while True:
            msg = await asyncio.wait_for(self.replies.get(), timeout)
            if msg.get("cmd") == cmd and "ok" in msg:
                return msg, items
            items.append(msg)


async def ask(prompt, default=None, secret=False):
    shown = f"{prompt} [{default}]: " if default else f"{prompt}: "
    fn = getpass.getpass if secret else input
    val = await asyncio.to_thread(fn, shown)
    val = val.strip()
    return val or (default or "")


def check(reply, what):
    if reply.get("ok"):
        print(f"  OK  {what}")
        return True
    print(f"  FAILED  {what}: {reply.get('err')}")
    return False


async def show_status(a):
    r, _ = await a.call("status")
    print(f"  device   : {r.get('device')}  (firmware {r.get('fw')})")
    print(f"  Wi-Fi    : {r.get('ssid') or '(not set)'}  "
          f"{'connected, %s dBm' % r.get('rssi') if r.get('wifi_connected') else 'not connected'}")
    print(f"  class    : {r.get('class') or '-'}  section: {r.get('section') or '-'}")
    print(f"  clock    : {r.get('time')}")
    print(f"  teachers : {r.get('teachers')}")


async def setup_wizard(a):
    r, _ = await a.call("status")
    ssid = await ask("Wi-Fi name (SSID)", r.get("ssid") or None)
    pw = await ask("Wi-Fi password (blank for an open network)", secret=True)
    cls = await ask("Class", r.get("class") or None)
    sec = await ask("Section", r.get("section") or None)
    if not (ssid and cls and sec):
        print("  SSID, class and section are required.")
        return
    check((await a.call("set_wifi", ssid=ssid, **{"pass": pw}))[0], "Wi-Fi saved")
    check((await a.call("set_class", **{"class": cls, "section": sec}))[0], "class and section saved")
    r, _ = await a.call("set_time", epoch=int(time.time()))
    check(r, f"clock set ({r.get('time')})")
    print("  The device saves these permanently. It will also sync the clock over Wi-Fi (NTP).")


async def add_by_tap(a):
    while not a.taps.empty():
        a.taps.get_nowait()
    print("  Tap the teacher's card on the device now (60 s)...")
    try:
        uid = await asyncio.wait_for(a.taps.get(), 60)
    except asyncio.TimeoutError:
        print("  No card tapped.")
        return
    print(f"  Card UID: {uid}")
    tid = await ask("  Teacher ID (used in file names, e.g. T001)")
    name = await ask("  Teacher name (shown on the display)")
    qr = await ask("  Teacher QR token (for uploads; leave empty to keep the current one)", secret=True)
    check((await a.call("add_teacher", uid=uid, id=tid, name=name, qr=qr))[0], f"{name or uid} added")


async def import_csv(a):
    path = await ask("  CSV file (columns: uid,id,name,qr)")
    try:
        with open(path, newline="", encoding="utf-8-sig") as f:
            rows = [r for r in csv.reader(f) if r and any(c.strip() for c in r)]
    except OSError as e:
        print("  Cannot read file:", e)
        return
    if rows and rows[0][0].strip().lower() == "uid":
        rows = rows[1:]
    ok = 0
    for r in rows:
        r = [c.strip() for c in r] + ["", "", "", ""]
        reply, _ = await a.call("add_teacher", uid=r[0], id=r[1], name=r[2], qr=r[3])
        if reply.get("ok"):
            ok += 1
        else:
            print(f"  skipped {r[0]}: {reply.get('err')}")
    print(f"  {ok} of {len(rows)} teachers imported.")


async def list_teachers(a):
    r, items = await a.call("list_teachers")
    print(f"  {r.get('count', 0)} teacher(s):")
    for m in items:
        t = m.get("teacher", {})
        print(f"    {t.get('uid',''):<22} {t.get('id',''):<14} {t.get('name',''):<24} "
              f"{'token set' if t.get('qr') else 'NO TOKEN (cannot upload)'}")


async def remove_teacher(a):
    uid = await ask("  UID to remove (or press Enter and tap the card on the device)")
    if not uid:
        while not a.taps.empty():
            a.taps.get_nowait()
        print("  Tap the card now (30 s)...")
        try:
            uid = await asyncio.wait_for(a.taps.get(), 30)
        except asyncio.TimeoutError:
            print("  No card tapped.")
            return
    check((await a.call("remove_teacher", uid=uid))[0], f"{uid} removed")


MENU = """
  1  Setup: Wi-Fi, class/section, clock
  2  Add a teacher (tap their card)
  3  Import teachers from a CSV file
  4  List teachers
  5  Remove a teacher
  6  Clear ALL teachers
  7  Device status
  8  Exit admin mode on the device and quit
  q  Quit (device stays in admin mode)
"""


async def find_device(name):
    print("Scanning for the device (hold the button 2 s so it shows its Bluetooth name)...")
    found = await BleakScanner.discover(timeout=8.0)
    cands = [d for d in found if d.name and d.name.startswith(NAME_PREFIX)]
    if name:
        cands = [d for d in cands if d.name == name]
    if not cands:
        return None
    if len(cands) == 1:
        return cands[0]
    for i, d in enumerate(cands, 1):
        print(f"  {i}. {d.name}  ({d.address})")
    idx = int(await ask("Which one", "1")) - 1
    return cands[idx]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", help="exact device name, e.g. TCHR-REC-4F2A")
    args = ap.parse_args()

    dev = await find_device(args.name)
    if not dev:
        print("No device found. Is it in admin mode (screen shows the Bluetooth name)?")
        return
    print(f"Connecting to {dev.name} ...")
    async with BleakClient(dev) as client:
        a = Admin(client)
        await client.start_notify(RESP_UUID, a.on_notify)
        await asyncio.sleep(0.5)
        await show_status(a)
        actions = {"1": setup_wizard, "2": add_by_tap, "3": import_csv, "4": list_teachers,
                   "5": remove_teacher, "7": show_status}
        while True:
            print(MENU)
            choice = (await ask("Choose")).lower()
            try:
                if choice in actions:
                    await actions[choice](a)
                elif choice == "6":
                    if (await ask("  Type YES to delete every teacher")) == "YES":
                        check((await a.call("clear_teachers"))[0], "teacher list cleared")
                elif choice == "8":
                    await a.call("exit")
                    print("  Device left admin mode.")
                    break
                elif choice == "q":
                    break
            except asyncio.TimeoutError:
                print("  The device did not answer in time.")


if __name__ == "__main__":
    asyncio.run(main())
