#!/usr/bin/env python3
"""Convert ADC loopback capture to WAV.

Usage:
  idf.py monitor | tee capture.txt     # run test, Ctrl-] to quit
  python3 adc_to_wav.py capture.txt     # creates captured.wav
"""
import wave, struct, sys

if len(sys.argv) < 2:
    print("Usage: python3 adc_to_wav.py <serial_log.txt>")
    sys.exit(1)

rate = 8000
values = []
capture = False

for line in open(sys.argv[1]):
    line = line.strip()
    if line.startswith("===ADC_START:"):
        rate = int(line.split(":")[1].rstrip("="))
        capture = True
        continue
    if line == "===ADC_END===":
        break
    if capture:
        try:
            values.append(int(line))
        except ValueError:
            pass

if not values:
    print("No ADC data found between markers.")
    sys.exit(1)

# Center around zero, scale 12-bit ADC range to 16-bit audio
mid = sum(values) / len(values)
samples = [max(-32768, min(32767, int((v - mid) * 16))) for v in values]

with wave.open("captured.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(struct.pack(f"<{len(samples)}h", *samples))

print(f"Wrote {len(samples)} samples at {rate} Hz to captured.wav")
