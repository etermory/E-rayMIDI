// Host-side integration test of the exact shared Android transport implementation.
#include "PcAudio.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static int connectPhone() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(49155);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    for (int i = 0; i < 200 && eray::pcAudioStatus() != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(eray::pcAudioStatus() == 2);
    return fd;
}

static uint32_t get32(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

static void checkPacket(int fd, int rate, int frames) {
    std::array<uint8_t, 16 + 512 * 4> data{};
    const int size = 16 + frames * 4;
    int received = 0;
    while (received < size) {
        auto n = recv(fd, data.data() + received, size - received, 0);
        assert(n > 0);
        received += n;
    }
    assert(std::memcmp(data.data(), "ERA1", 4) == 0);
    assert(get32(data.data() + 4) == unsigned(rate));
    assert(get32(data.data() + 8) == unsigned(frames));
    assert(data[12] == 2 && data[14] == 16);
    for (int i = 0; i < frames; ++i) {
        assert(data[16 + i * 4] == 0x34 && data[17 + i * 4] == 0x12);
        assert(data[18 + i * 4] == 0xcc && data[19 + i * 4] == 0xed);
    }
}

int main() {
    assert(!eray::setPcAudioMode(3));
    assert(eray::setPcAudioMode(1));
    assert(eray::pcAudioStatus() == 1);
    int client = connectPhone();
    std::array<int16_t, 1200> pcm;
    auto fill = [&] { for (int i = 0; i < 600; ++i) { pcm[i*2] = 0x1234; pcm[i*2+1] = -0x1234; } };
    fill();
    eray::forwardPcAudio(pcm.data(), 600, 66207);
    checkPacket(client, 66207, 512);
    checkPacket(client, 66207, 88);
    assert(pcm[0] == 0x1234);
    assert(eray::setPcAudioMode(2));
    eray::forwardPcAudio(pcm.data(), 600, 32000);
    checkPacket(client, 32000, 512);
    checkPacket(client, 32000, 88);
    for (auto sample : pcm) assert(sample == 0);
    close(client);
    for (int i = 0; i < 200 && eray::pcAudioStatus() == 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(eray::pcAudioStatus() == 1);
    client = connectPhone();
    fill();
    eray::forwardPcAudio(pcm.data(), 600, 48000);
    checkPacket(client, 48000, 512);
    checkPacket(client, 48000, 88);
    // A stalled PC must never block the real-time callback or shutdown.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) eray::forwardPcAudio(pcm.data(), 600, 48000);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(eray::setPcAudioMode(0));
    assert(eray::pcAudioStatus() == 0);
    close(client);
    // Listener and worker can be started again after stopping.
    assert(eray::setPcAudioMode(1));
    client = connectPhone();
    fill();
    eray::forwardPcAudio(pcm.data(), 600, 44100);
    checkPacket(client, 44100, 512);
    checkPacket(client, 44100, 88);
    eray::setPcAudioMode(0);
    close(client);
    std::cout << "Transport integration checks passed\n";
}
