import socket
import threading
import unittest
from unittest.mock import patch

import numpy as np
from receiver import AudioBuffer, Converter, HEADER, UsbForward, read_packet


class ReceiverTests(unittest.TestCase):
    def test_fragmented_packet_and_channel_order(self):
        left, right = socket.socketpair()
        pcm = np.array([[32767, -32768], [1234, -4321]], dtype="<i2")
        packet = HEADER.pack(b"ERA1", 66207, 2, 2, 16) + pcm.tobytes()

        def send():
            with left:
                for byte in packet:
                    left.sendall(bytes([byte]))

        worker = threading.Thread(target=send)
        worker.start()
        with right:
            rate, result = read_packet(right)
        worker.join()
        self.assertEqual(rate, 66207)
        np.testing.assert_array_equal(result * 32768, pcm)

    def test_reject_invalid_header_and_truncated_pcm(self):
        for packet, error in [
            (HEADER.pack(b"BAD!", 48000, 1, 2, 16), ValueError),
            (HEADER.pack(b"ERA1", 48000, 513, 2, 16), ValueError),
            (HEADER.pack(b"ERA1", 48000, 1, 1, 16), ValueError),
            (HEADER.pack(b"ERA1", 48000, 1, 2, 16) + b"\0", EOFError),
        ]:
            with self.subTest(error=error), self._packet_socket(packet) as sock:
                with self.assertRaises(error):
                    read_packet(sock)

    @staticmethod
    def _packet_socket(packet):
        sender, receiver = socket.socketpair()
        sender.sendall(packet)
        sender.close()
        return receiver

    def test_jitter_buffer_waits_then_recovers_from_underrun(self):
        buffer = AudioBuffer(1000, 20)
        out = np.ones((15, 2), dtype=np.float32)
        buffer.push(np.ones((10, 2), dtype=np.float32))
        buffer.callback(out, 15, None, None)
        self.assertFalse(out.any())
        buffer.push(np.ones((10, 2), dtype=np.float32))
        buffer.callback(out, 15, None, None)
        self.assertTrue((out == 1).all())
        buffer.callback(out, 15, None, None)
        self.assertTrue((out[:5] == 1).all())
        self.assertFalse(out[5:].any())
        self.assertEqual(buffer.underruns, 1)
        self.assertFalse(buffer.ready)

    def test_overflow_retains_newest_audio_and_clear_removes_old_engine(self):
        buffer = AudioBuffer(1000, 20)
        buffer.push(np.repeat(np.arange(100, dtype=np.float32)[:, None], 2, axis=1))
        self.assertEqual(buffer.frames, 20)
        out = np.zeros((20, 2), dtype=np.float32)
        buffer.callback(out, 20, None, None)
        np.testing.assert_array_equal(out[:, 0], np.arange(80, 100))
        buffer.push(np.ones((20, 2), dtype=np.float32))
        buffer.clear()
        buffer.callback(out, 20, None, None)
        self.assertFalse(out.any())

    def test_all_engine_rates_preserve_tone_and_duration(self):
        for source_rate in (32000, 44100, 48000, 66207):
            with self.subTest(rate=source_rate):
                converter = Converter(source_rate, 48000)
                buffer = AudioBuffer(48000, 60) # empty: no simulated clock correction
                wave = np.sin(2 * np.pi * 1000 * np.arange(source_rate) / source_rate).astype(np.float32)
                stereo = np.column_stack((wave, -wave))
                chunks = [converter.process(stereo[i:i + 512], buffer) for i in range(0, source_rate, 512)]
                chunks.append(converter.stream.resample_chunk(np.empty((0, 2), dtype=np.float32), last=True))
                result = np.concatenate(chunks)
                self.assertLess(abs(len(result) - 48000), 3)
                np.testing.assert_allclose(result[:, 0], -result[:, 1], atol=1e-5)
                frequency = np.argmax(abs(np.fft.rfft(result[:, 0]))) * 48000 / len(result)
                self.assertLess(abs(frequency - 1000), 2)

    def test_forward_requires_usb_and_midi(self):
        forward = UsbForward("adb")
        with patch.object(forward, "command", return_value="unknown"):
            with self.assertRaisesRegex(RuntimeError, "USB phone"):
                forward.open()
        with patch.object(forward, "command", side_effect=["phone", "mtp,adb"]):
            with self.assertRaisesRegex(RuntimeError, "MIDI"):
                forward.open()

    def test_usb_selection_without_devpath(self):
        import subprocess
        forward = UsbForward("adb")
        outputs = ["phone", "midi,adb", "54321", ""]
        with patch("receiver.subprocess.run", side_effect=[
            subprocess.CompletedProcess([], 0, stdout=value) for value in outputs
        ]) as run:
            forward.open()
            forward.close()
            self.assertEqual(run.call_args_list[0].args[0], ["adb", "-d", "get-serialno"])
            for call in run.call_args_list[1:]:
                self.assertEqual(call.args[0][:2], ["adb", "-d"])
            self.assertNotIn("get-devpath", str(run.call_args_list))

    def test_forward_allocates_and_removes_only_its_own_port(self):
        forward = UsbForward("adb")
        with patch.object(forward, "command", side_effect=["phone", "midi,adb", "54321", ""]) as command:
            self.assertEqual(forward.open(), 54321)
            forward.close()
            self.assertEqual(command.call_args_list[-2].args, ("forward", "tcp:0", "tcp:49155"))
            self.assertEqual(command.call_args_list[-1].args, ("forward", "--remove", "tcp:54321"))


if __name__ == "__main__":
    unittest.main()
