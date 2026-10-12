#pragma once
#include <cstdint>

namespace eray {
// Called by each engine's AAudio callback. No allocation, locks or socket I/O.
// PCM is copied before optional local muting; the engine's sample rate is unchanged.
void forwardPcAudio(int16_t* stereo, int frames, int sampleRate);
// 0: phone, 1: phone + PC, 2: PC only. Control calls must be serialized.
bool setPcAudioMode(int mode);
int pcAudioStatus(); // 0: off, 1: waiting, 2: connected
}
