#!/usr/bin/env python3
"""
Kai Car — BLE PC controller with dynamic speed.

Arrow keys / WASD to drive, Space to stop, Q to quit.
[ / ] to adjust speed by 5%.  Number keys 5-9 for presets.

Sends commands every ~200 ms over BLE, keeping the
ESP32 deadman watchdog alive so motors run continuously.

Requires: pip install bleak
"""

import asyncio
import curses
import threading

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "KaiCar"
SERVICE_UUID = "000000ff-0000-1000-8000-00805f9b34fb"
CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
SEND_INTERVAL = 0.20

MIN_SPEED = 65
MAX_SPEED = 100
DEFAULT_SPEED = 80

# ── key mappings ─────────────────────────────────────────────────────
KEY_MAP = {
    curses.KEY_UP:    "forward",
    curses.KEY_DOWN:  "reverse",
    curses.KEY_LEFT:  "left",
    curses.KEY_RIGHT: "right",
    ord("w"):         "forward",
    ord("W"):         "forward",
    ord("a"):         "left",
    ord("A"):         "left",
    ord("d"):         "right",
    ord("D"):         "right",
    ord("x"):         "reverse",
    ord("X"):         "reverse",
    ord("s"):         "stop",
    ord("S"):         "stop",
    ord(" "):         "stop",
}

COMMAND_LABEL = {
    "forward": "FORWARD",
    "reverse": "REVERSE",
    "left":    "LEFT",
    "right":   "RIGHT",
    "stop":    "STOP",
}


# ── BLE sender (background thread with asyncio) ─────────────────────
class BleController:
    def __init__(self):
        self._command = "stop"
        self._speed = DEFAULT_SPEED
        self._honk_pending = False
        self._lock = threading.Lock()
        self._running = True
        self._status = "scanning..."
        self._send_count = 0
        self._error_count = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def set_command(self, cmd):
        with self._lock:
            self._command = cmd

    def get_command(self):
        with self._lock:
            return self._command

    def set_speed(self, speed):
        with self._lock:
            self._speed = max(MIN_SPEED, min(MAX_SPEED, speed))

    def get_speed(self):
        with self._lock:
            return self._speed

    def adjust_speed(self, delta):
        with self._lock:
            self._speed = max(MIN_SPEED, min(MAX_SPEED, self._speed + delta))

    def honk(self):
        with self._lock:
            self._honk_pending = True

    @property
    def status(self):
        with self._lock:
            return self._status

    @property
    def send_count(self):
        return self._send_count

    @property
    def error_count(self):
        return self._error_count

    def stop(self):
        self._running = False

    def _run(self):
        asyncio.run(self._ble_loop())

    async def _ble_loop(self):
        while self._running:
            try:
                with self._lock:
                    self._status = "scanning..."

                device = await BleakScanner.find_device_by_name(
                    DEVICE_NAME, timeout=5.0
                )
                if device is None:
                    with self._lock:
                        self._status = "KaiCar not found"
                    self._error_count += 1
                    await asyncio.sleep(2.0)
                    continue

                async with BleakClient(device) as client:
                    with self._lock:
                        self._status = "connected"

                    while self._running and client.is_connected:
                        # Check for pending honk
                        send_honk = False
                        with self._lock:
                            if self._honk_pending:
                                send_honk = True
                                self._honk_pending = False

                        cmd = self.get_command()
                        speed = self.get_speed()

                        try:
                            if send_honk:
                                await client.write_gatt_char(
                                    CHAR_UUID, b"honk:0", response=False
                                )
                                self._send_count += 1

                            payload = f"{cmd}:{speed}".encode()
                            await client.write_gatt_char(
                                CHAR_UUID, payload, response=False
                            )
                            self._send_count += 1
                            with self._lock:
                                self._status = "connected"
                        except Exception:
                            self._error_count += 1
                            with self._lock:
                                self._status = "write error"
                            break

                        await asyncio.sleep(SEND_INTERVAL)

                with self._lock:
                    self._status = "disconnected"

            except Exception as e:
                self._error_count += 1
                with self._lock:
                    self._status = str(e)[:30]
                await asyncio.sleep(2.0)

        # send final stop on exit
        try:
            device = await BleakScanner.find_device_by_name(
                DEVICE_NAME, timeout=2.0
            )
            if device:
                async with BleakClient(device) as client:
                    await client.write_gatt_char(
                        CHAR_UUID, b"stop:0", response=False
                    )
        except Exception:
            pass


# ── curses UI ────────────────────────────────────────────────────────
def main(stdscr):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(100)

    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_GREEN, -1)
        curses.init_pair(2, curses.COLOR_RED, -1)
        curses.init_pair(3, curses.COLOR_CYAN, -1)
        curses.init_pair(4, curses.COLOR_YELLOW, -1)

    ctl = BleController()

    try:
        while True:
            key = stdscr.getch()

            if key in (ord("q"), ord("Q")):
                ctl.set_command("stop")
                break

            if key in KEY_MAP:
                ctl.set_command(KEY_MAP[key])
            elif key in (ord("h"), ord("H")):
                ctl.honk()
            elif key >= ord("0") and key <= ord("3"):
                ctl.set_command(f"pin{chr(key)}")
            elif key == ord("]") or key == ord("="):
                ctl.adjust_speed(5)
            elif key == ord("[") or key == ord("-"):
                ctl.adjust_speed(-5)
            elif ord("5") <= key <= ord("9"):
                ctl.set_speed((key - ord("0")) * 10 + 10)

            # ── draw ─────────────────────────────────────────
            stdscr.erase()
            h, w = stdscr.getmaxyx()

            cmd = ctl.get_command()
            speed = ctl.get_speed()
            status = ctl.status
            sent = ctl.send_count
            errs = ctl.error_count

            def put(r, c, text, attr=0):
                if 0 <= r < h - 1 and 0 <= c < w:
                    try:
                        stdscr.addstr(r, c, text[: w - c - 1], attr)
                    except curses.error:
                        pass

            row = 1
            put(row, 2, "KAI CAR — BLE CONTROL", curses.A_BOLD)
            row += 2

            sc = curses.color_pair(1) if status == "connected" else curses.color_pair(2)
            put(row, 2, "Link: ")
            put(row, 8, status, sc | curses.A_BOLD)
            row += 1
            put(row, 2, f"Sent: {sent}  Errors: {errs}")
            row += 2

            label = COMMAND_LABEL.get(cmd, cmd)
            cc = curses.color_pair(3) if cmd != "stop" else curses.color_pair(4)
            put(row, 2, "Command: ", curses.A_BOLD)
            put(row, 11, label, cc | curses.A_BOLD)
            row += 2

            # speed bar
            bar_len = 20
            filled = int((speed - MIN_SPEED) / (MAX_SPEED - MIN_SPEED) * bar_len)
            bar = "#" * filled + "-" * (bar_len - filled)
            put(row, 2, "Speed:  ", curses.A_BOLD)
            put(row, 10, f"[{bar}] {speed}%", curses.color_pair(3))
            row += 2

            # d-pad
            def pad(active):
                return (curses.A_REVERSE | curses.A_BOLD) if active else curses.A_DIM

            put(row,     10, " ^ ", pad(cmd == "forward"))
            row += 1
            put(row,     4,  " < ", pad(cmd == "left"))
            put(row,     10, "STP", pad(cmd == "stop"))
            put(row,     16, " > ", pad(cmd == "right"))
            row += 1
            put(row,     10, " v ", pad(cmd == "reverse"))
            row += 2

            put(row, 2, "WASD=drive [/]=speed H=honk Q=quit")
            row += 1
            put(row, 2, "0-3=test individual motor pins")

            stdscr.refresh()
    finally:
        ctl.stop()


if __name__ == "__main__":
    curses.wrapper(main)
