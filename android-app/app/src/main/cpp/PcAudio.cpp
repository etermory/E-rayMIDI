#include "PcAudio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

namespace {
constexpr int kPort = 49155;
constexpr unsigned kSlots = 16;
constexpr int kFrames = 512;
struct Packet {
    uint32_t rate = 0, frames = 0;
    std::array<int16_t, kFrames * 2> pcm{};
};
std::array<Packet, kSlots> queue;
std::atomic<unsigned> head{0}, tail{0};
std::atomic<int> mode{0};
std::atomic<bool> running{false}, connected{false}, overflow{false};
// Engine transitions should be serialized, but never block if callbacks overlap.
std::atomic_flag producer = ATOMIC_FLAG_INIT;
std::thread sender;

void put32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

bool sendPacket(int fd, const Packet& packet) {
    // Wire format: ERA1, rate:u32, frames:u32, channels:u16, bits:u16, PCM S16LE.
    std::array<uint8_t, 16 + kFrames * 4> bytes{};
    std::memcpy(bytes.data(), "ERA1", 4);
    put32(bytes.data() + 4, packet.rate);
    put32(bytes.data() + 8, packet.frames);
    bytes[12] = 2;
    bytes[14] = 16;
    for (unsigned i = 0; i < packet.frames * 2; ++i) {
        const auto value = static_cast<uint16_t>(packet.pcm[i]);
        bytes[16 + 2 * i] = static_cast<uint8_t>(value);
        bytes[17 + 2 * i] = static_cast<uint8_t>(value >> 8);
    }
    size_t sent = 0, size = 16 + packet.frames * 4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (sent < size && running.load() && !overflow.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        auto count = send(fd, bytes.data() + sent, size - sent, flags);
        if (count > 0) { sent += static_cast<size_t>(count); continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            poll(&p, 1, 10);
            continue;
        }
        return false;
    }
    return sent == size;
}

void serve(int listener) {
    while (running.load()) {
        pollfd p{listener, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        int client = accept(listener, nullptr, nullptr);
        if (client < 0) continue;
        fcntl(client, F_SETFL, O_NONBLOCK);
        int one = 1, buffer = 8192;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
#ifdef SO_NOSIGPIPE
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        // Drain only on this consumer thread. In-flight packets from the previous
        // connection are harmless and bounded; never reset producer indices live.
        tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        overflow.store(false);
        connected.store(true);
        while (running.load() && !overflow.load()) {
            unsigned t = tail.load(std::memory_order_relaxed);
            if (t == head.load(std::memory_order_acquire)) {
                char dummy;
                auto n = recv(client, &dummy, 1, MSG_PEEK);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!sendPacket(client, queue[t % kSlots])) break;
            tail.store(t + 1, std::memory_order_release);
        }
        connected.store(false);
        close(client);
    }
    close(listener);
}
}

namespace eray {
void forwardPcAudio(int16_t* stereo, int frames, int sampleRate) {
    int currentMode = mode.load(std::memory_order_relaxed);
    if (!currentMode || frames <= 0) return;
    if (connected.load(std::memory_order_acquire) && !producer.test_and_set(std::memory_order_acquire)) {
        for (int offset = 0; offset < frames; offset += kFrames) {
            unsigned h = head.load(std::memory_order_relaxed);
            if (h - tail.load(std::memory_order_acquire) >= kSlots) {
                overflow.store(true); // Reconnect instead of accumulating stale audio.
                break;
            }
            auto& packet = queue[h % kSlots];
            packet.rate = sampleRate;
            packet.frames = std::min(kFrames, frames - offset);
            std::memcpy(packet.pcm.data(), stereo + offset * 2, packet.frames * 4);
            head.store(h + 1, std::memory_order_release);
        }
        producer.clear(std::memory_order_release);
    }
    if (currentMode == 2) std::memset(stereo, 0, static_cast<size_t>(frames) * 4);
}

bool setPcAudioMode(int next) {
    if (next < 0 || next > 2) return false;
    if (next == 0) {
        mode.store(0);
        running.store(false);
        if (sender.joinable()) sender.join();
        connected.store(false);
        return true;
    }
    if (running.load()) { mode.store(next); return true; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // USB adb forward only; no LAN listener.
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(fd, 1)) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    running.store(true);
    try { sender = std::thread(serve, fd); }
    catch (...) { running.store(false); close(fd); return false; }
    mode.store(next);
    return true;
}

int pcAudioStatus() { return mode.load() == 0 ? 0 : (connected.load() ? 2 : 1); }
}

#ifdef __ANDROID__
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_nukedsc55_PcAudio_nativeSetMode(JNIEnv*, jobject, jint value) {
    return eray::setPcAudioMode(value);
}
extern "C" JNIEXPORT jint JNICALL
Java_com_example_nukedsc55_PcAudio_nativeStatus(JNIEnv*, jobject) {
    return eray::pcAudioStatus();
}
#endif
