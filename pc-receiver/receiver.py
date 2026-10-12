"""E-ray MIDI PCM receiver. ADB forwarding is restricted to a USB device."""
import argparse
from collections import deque
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import threading
import time

import numpy as np
import soxr

HEADER = struct.Struct("<4sIIHH")
PHONE_PORT = 49155


def read_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError("Phone disconnected")
        result.extend(chunk)
    return result


def read_packet(sock):
    magic, rate, frames, channels, bits = HEADER.unpack(read_exact(sock, HEADER.size))
    if magic != b"ERA1" or not 8000 <= rate <= 192000 or not 1 <= frames <= 512 or (channels, bits) != (2, 16):
        raise ValueError("Invalid E-ray audio packet; update the app and receiver together")
    samples = np.frombuffer(read_exact(sock, frames * 4), dtype="<i2")
    return rate, samples.reshape(-1, 2).astype(np.float32) / 32768.0


class AudioBuffer:
    """Bounded jitter buffer; device callback never waits for the USB connection."""
    def __init__(self, rate, buffer_ms):
        self.target = max(1, int(rate * buffer_ms / 1000))
        self.limit = self.target * 3
        self.chunks = deque()
        self.frames = 0
        self.ready = False
        self.lock = threading.Lock()
        self.underruns = 0
        self.overruns = 0

    def clear(self):
        with self.lock:
            self.chunks.clear()
            self.frames = 0
            self.ready = False

    def level(self):
        with self.lock:
            return self.frames, self.ready

    def push(self, samples):
        if not len(samples):
            return
        with self.lock:
            self.chunks.append(samples)
            self.frames += len(samples)
            if self.frames > self.limit:
                self.overruns += 1
                # Drop old audio, retaining the newest target-sized window.
                while self.frames > self.target and self.chunks:
                    first = self.chunks.popleft()
                    drop = min(len(first), self.frames - self.target)
                    self.frames -= drop
                    if drop < len(first):
                        self.chunks.appendleft(first[drop:])
            if self.frames >= self.target:
                self.ready = True

    def callback(self, outdata, frames, _time, _status):
        outdata.fill(0)
        # Keep the sound-device callback bounded even if the receive thread is busy.
        if not self.lock.acquire(blocking=False):
            return
        try:
            if not self.ready:
                return
            copied = 0
            while copied < frames and self.chunks:
                chunk = self.chunks.popleft()
                count = min(frames - copied, len(chunk))
                outdata[copied:copied + count] = chunk[:count]
                copied += count
                self.frames -= count
                if count < len(chunk):
                    self.chunks.appendleft(chunk[count:])
            if copied < frames:
                self.underruns += 1
                self.ready = False
        finally:
            self.lock.release()


class Converter:
    """Phone PCM stays native; conversion and clock drift correction happen on PC."""
    def __init__(self, source_rate, output_rate):
        self.source_rate = source_rate
        self.output_rate = output_rate
        self.correction = 0.0
        self.stream = soxr.ResampleStream(source_rate * 1.005, output_rate, 2,
                                          dtype="float32", quality="HQ", vr=True)
        self.stream.set_io_ratio(source_rate, output_rate)

    def process(self, samples, buffer):
        level, ready = buffer.level()
        # Smoothly follow the two independent audio clocks (at most +/-0.3%).
        desired = np.clip((level - buffer.target) / buffer.target * 0.001, -0.003, 0.003) if ready else 0.0
        self.correction += (float(desired) - self.correction) * 0.01
        self.stream.set_io_ratio(self.source_rate * (1 + self.correction), self.output_rate, len(samples))
        return self.stream.resample_chunk(samples)


class UsbForward:
    def __init__(self, adb, serial=None):
        self.adb = adb
        self.serial = serial
        self.port = None

    def command(self, *args):
        selector = ["-d"]
        result = subprocess.run([self.adb, *selector, *args], capture_output=True,
                                text=True, timeout=10, check=True)
        return result.stdout.strip()

    def open(self):
        # Windows USB transports need not have a devpath. ADB -d selects USB.
        serial = self.command("get-serialno")
        if not serial or serial == "unknown":
            raise RuntimeError("No authorized USB phone")
        if self.serial and serial != self.serial:
            raise RuntimeError("USB phone serial does not match --serial. Connect only the requested USB phone.")
        state = self.command("shell", "getprop", "sys.usb.state")
        if "midi" not in state.split(","):
            raise RuntimeError("Set the phone USB mode to MIDI and keep USB debugging enabled (current: " + state + ")")
        # ADB allocates a free PC port; never replace another app's forwarding rule.
        self.port = int(self.command("forward", "tcp:0", f"tcp:{PHONE_PORT}"))
        return self.port

    def close(self):
        if self.port is not None:
            try:
                self.command("forward", "--remove", f"tcp:{self.port}")
            except (OSError, subprocess.SubprocessError):
                pass
            self.port = None


def receive_loop(args, buffer, output_rate, stop):
    last_message = None
    while not stop.is_set():
        forward = UsbForward(args.adb, args.serial)
        try:
            port = forward.open()
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(3)
                converter = None
                last_message = None
                while not stop.is_set():
                    rate, samples = read_packet(sock)
                    if converter is None or converter.source_rate != rate:
                        buffer.clear()
                        converter = Converter(rate, output_rate)
                        print(f"Connected: phone {rate} Hz -> PC {output_rate} Hz, stereo. Ctrl+C to stop.", flush=True)
                    buffer.push(converter.process(samples, buffer))
        except (OSError, EOFError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            detail = getattr(error, "stderr", None) or str(error)
            message = f"Waiting for USB audio: {detail.strip()}"
            if message != last_message:
                print(message, flush=True)
                last_message = message
        finally:
            buffer.clear()
            forward.close()
        stop.wait(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", help="Path to adb.exe (or put platform-tools beside this script)")
    parser.add_argument("--serial", help="Verify the connected USB phone serial (connect only one USB phone)")
    parser.add_argument("--device", help="PC output device index or name")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--buffer-ms", type=int, default=60, help="Jitter buffer, 20..500 ms (default 60)")
    args = parser.parse_args()
    import sounddevice as sd
    if args.list_devices:
        print(sd.query_devices())
        return
    if not 20 <= args.buffer_ms <= 500:
        parser.error("--buffer-ms must be between 20 and 500")
    if not args.adb:
        bundled = Path(__file__).parent / "platform-tools" / "adb.exe"
        args.adb = str(bundled) if bundled.is_file() else shutil.which("adb")
    if not args.adb:
        parser.error("adb.exe not found. Extract Android platform-tools beside receiver.py, or pass --adb PATH")
    device = int(args.device) if args.device and args.device.isdigit() else args.device
    output_rate = int(sd.query_devices(device, "output")["default_samplerate"])
    buffer = AudioBuffer(output_rate, args.buffer_ms)
    stop = threading.Event()
    receiver = threading.Thread(target=receive_loop, args=(args, buffer, output_rate, stop), daemon=True)
    try:
        with sd.OutputStream(device=device, samplerate=output_rate, channels=2, dtype="float32",
                             latency="low", callback=buffer.callback):
            receiver.start()
            print("Select PC output in the phone app and start its engine. Waiting for USB MIDI + ADB...", flush=True)
            while receiver.is_alive():
                time.sleep(0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if receiver.ident is not None:
            receiver.join(timeout=15)
        print(f"Stopped. Buffer underruns: {buffer.underruns}, overruns: {buffer.overruns}")


if __name__ == "__main__":
    main()
