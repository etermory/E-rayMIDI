#include "audio_core.h"
#include "usb_selection.h"
#include <array>
#include <cassert>
#include <iostream>

int main() {
    eray::UsbSelection usb{};
    int calls = 0;
    usb.select([&](std::vector<std::wstring> args) {
        ++calls;
        assert(args == std::vector<std::wstring>{L"get-serialno"});
        assert(usb.selector() == std::vector<std::wstring>{L"-d"});
        return std::string("phone");
    });
    assert(calls == 1);
    usb.requestedSerial = L"phone";
    usb.select([](auto) { return std::string("phone"); });
    assert(usb.selector() == std::vector<std::wstring>{L"-d"});
    for (const auto& bad : {"", "unknown", "other-phone"}) {
        bool rejected = false;
        try { usb.select([&](auto) { return std::string(bad); }); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    std::array<uint8_t, 16> packet{'E','R','A','1', 0x9f, 0x02, 1, 0, 2,0,0,0, 2,0,16,0};
    auto h = eray::parseHeader(packet.data());
    assert(h.rate == 66207 && h.frames == 2);
    for (int offset : {0, 11, 12, 13, 14, 15}) {
        auto bad = packet; bad[offset] = 255;
        bool rejected = false;
        try { eray::parseHeader(bad.data()); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert(eray::quote(L"C:\\Program Files\\adb.exe") == L"\"C:\\Program Files\\adb.exe\"");
    assert(eray::quote(L"end\\") == L"\"end\\\\\"");
    assert(eray::quote(L"a\"b") == L"\"a\\\"b\"");
    assert(eray::quote(L"") == L"\"\"");
    eray::AudioQueue queue(20);
    queue.push(1000, std::vector<int16_t>(20, 3));
    auto state = queue.state();
    assert(!state.ready);
    int16_t out[40];
    queue.read(out, 20, state.generation);
    for (auto v : out) assert(v == 0);
    queue.push(1000, std::vector<int16_t>(20, 5));
    queue.read(out, 20, state.generation);
    for (int i = 0; i < 40; ++i) assert(out[i] == (i < 20 ? 3 : 5));
    queue.read(out, 20, state.generation);
    assert(!queue.state().ready);
    queue.push(1000, std::vector<int16_t>(400, 9));
    assert(queue.state().frames == 20); // overflow retains only the newest target window
    queue.push(2000, std::vector<int16_t>(80, 7));
    queue.read(out, 20, state.generation); // old engine generation must be silent
    for (auto v : out) assert(v == 0);
    state = queue.state();
    assert(state.rate == 2000 && state.ready);
    queue.read(out, 20, state.generation);
    for (auto v : out) assert(v == 7);
    queue.clear();
    assert(queue.state().rate == 0 && !queue.state().ready);
    queue.read(out, 20, state.generation);
    for (auto v : out) assert(v == 0);
    std::cout << "Native receiver core checks passed\n";
}
