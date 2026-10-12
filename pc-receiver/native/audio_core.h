#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace eray {
struct Header { uint32_t rate, frames; };
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline Header parseHeader(const uint8_t* p) {
    Header h{le32(p + 4), le32(p + 8)};
    if (std::memcmp(p, "ERA1", 4) || h.rate < 8000 || h.rate > 192000 ||
        !h.frames || h.frames > 512 || p[12] != 2 || p[13] || p[14] != 16 || p[15])
        throw std::runtime_error("Invalid audio packet. Update the phone app and receiver together.");
    return h;
}

// Microsoft CRT command-line quoting, including quotes and trailing backslashes.
inline std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

class AudioQueue {
    std::mutex mutex;
    std::deque<int16_t> samples;
    uint32_t rate = 0;
    unsigned generation = 0;
    bool ready = false;
    size_t target = 0;
    int bufferMs;
public:
    explicit AudioQueue(int ms) : bufferMs(ms) {}
    struct State { uint32_t rate; unsigned generation; size_t frames, target; bool ready; };
    State state() {
        std::lock_guard<std::mutex> lock(mutex);
        return {rate, generation, samples.size() / 2, target, ready};
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        samples.clear(); rate = 0; ready = false; ++generation;
    }
    void push(uint32_t newRate, const std::vector<int16_t>& data) {
        std::lock_guard<std::mutex> lock(mutex);
        if (newRate != rate) {
            samples.clear(); ready = false; rate = newRate; ++generation;
            target = std::max<size_t>(1, size_t(rate) * bufferMs / 1000);
        }
        samples.insert(samples.end(), data.begin(), data.end());
        if (samples.size() / 2 > target * 3)
            samples.erase(samples.begin(), samples.end() - static_cast<std::ptrdiff_t>(target * 2));
        if (samples.size() / 2 >= target) ready = true;
    }
    void read(int16_t* out, size_t frames, unsigned expectedGeneration) {
        std::fill_n(out, frames * 2, int16_t(0));
        std::lock_guard<std::mutex> lock(mutex);
        if (!ready || expectedGeneration != generation) return;
        size_t count = std::min(frames * 2, samples.size());
        for (size_t i = 0; i < count; ++i) { out[i] = samples.front(); samples.pop_front(); }
        if (count < frames * 2) ready = false;
    }
};
}
