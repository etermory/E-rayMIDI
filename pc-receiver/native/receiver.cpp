#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include "audio_core.h"
#include "usb_selection.h"

namespace fs = std::filesystem;
std::atomic<bool> stopping{false};
std::mutex logMutex;
void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << message << std::endl;
}
BOOL WINAPI onControl(DWORD) { stopping = true; return TRUE; }
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream s;
        s << operation << " failed (0x" << std::hex << uint32_t(result) << ").";
        throw std::runtime_error(s.str());
    }
}
template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T* operator->() { return p; }
};
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
struct Socket {
    SOCKET s = INVALID_SOCKET;
    ~Socket() { if (s != INVALID_SOCKET) closesocket(s); }
};
std::wstring wide(const std::string& value) {
    int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), n);
    return result;
}
std::string utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::string trim(std::string s) {
    auto end = s.find_last_not_of(" \r\n\t");
    if (end == std::string::npos) return {};
    return s.substr(s.find_first_not_of(" \r\n\t"), end - s.find_first_not_of(" \r\n\t") + 1);
}

fs::path extractAdb() {
    wchar_t local[32768];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA is unavailable.");
    // Resource-content version supplied at build time; old running ADB servers
    // are not overwritten. Files are byte-verified before every invocation.
    fs::path dir = fs::path(local) / L"E-rayMIDI" / L"adb-" ERAY_ADB_ID;
    fs::create_directories(dir);
    const wchar_t* names[] = {L"adb.exe", L"AdbWinApi.dll", L"AdbWinUsbApi.dll", L"NOTICE.txt"};
    for (int i = 0; i < 4; ++i) {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101 + i), MAKEINTRESOURCEW(10));
        if (!resource) throw std::runtime_error("Embedded ADB resource is missing.");
        auto size = SizeofResource(nullptr, resource);
        auto bytes = static_cast<const char*>(LockResource(LoadResource(nullptr, resource)));
        if (!bytes || !size) throw std::runtime_error("Embedded ADB resource is invalid.");
        fs::path dest = dir / names[i];
        bool same = false;
        if (fs::exists(dest) && fs::file_size(dest) == size) {
            std::ifstream in(dest, std::ios::binary);
            std::vector<char> current(size);
            in.read(current.data(), size);
            same = in.good() && std::memcmp(current.data(), bytes, size) == 0;
        }
        if (!same) {
            fs::path temporary = dest;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(bytes, size); out.close();
            if (!out || !MoveFileExW(temporary.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot prepare embedded ADB files in LOCALAPPDATA.");
        }
    }
    return dir / L"adb.exe";
}

struct CommandResult { DWORD code; std::string out, err; };
CommandResult run(const fs::path& executable, const std::vector<std::wstring>& args) {
    std::wstring command = eray::quote(executable.wstring());
    for (const auto& arg : args) command += L" " + eray::quote(arg);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle outRead, outWrite, errRead, errWrite, nullInput;
    if (!CreatePipe(&outRead.h, &outWrite.h, &security, 0) ||
        !CreatePipe(&errRead.h, &errWrite.h, &security, 0)) throw std::runtime_error("Cannot create ADB pipes.");
    SetHandleInformation(outRead.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead.h, HANDLE_FLAG_INHERIT, 0);
    nullInput.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite.h; si.hStdError = errWrite.h; si.hStdInput = nullInput.h;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, executable.parent_path().c_str(), &si, &pi))
        throw std::runtime_error("Cannot start bundled adb.exe (Windows error " + std::to_string(GetLastError()) + ").");
    Handle process{pi.hProcess}, thread{pi.hThread};
    CloseHandle(outWrite.h); outWrite.h = nullptr;
    CloseHandle(errWrite.h); errWrite.h = nullptr;
    CommandResult result{};
    auto drain = [](HANDLE pipe, std::string& output) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char data[4096]; DWORD count = 0;
            if (!ReadFile(pipe, data, std::min<DWORD>(available, sizeof(data)), &count, nullptr) || !count) break;
            if (output.size() < 65536) output.append(data, count);
        }
    };
    auto deadline = GetTickCount64() + 10000;
    while (true) {
        drain(outRead.h, result.out); drain(errRead.h, result.err);
        if (WaitForSingleObject(process.h, 20) == WAIT_OBJECT_0) break;
        if (GetTickCount64() > deadline) {
            TerminateProcess(process.h, 1); WaitForSingleObject(process.h, 1000);
            throw std::runtime_error("ADB command timed out. Check the phone's USB debugging prompt.");
        }
    }
    drain(outRead.h, result.out); drain(errRead.h, result.err);
    GetExitCodeProcess(process.h, &result.code);
    result.out = trim(result.out); result.err = trim(result.err);
    return result;
}

struct Forward {
    fs::path adb;
    eray::UsbSelection selection;
    int port = 0;
    Forward(fs::path path, std::wstring serial) : adb(std::move(path)), selection{std::move(serial)} {}
    std::string command(std::vector<std::wstring> args) {
        auto all = selection.selector();
        all.insert(all.end(), args.begin(), args.end());
        auto r = run(adb, all);
        if (r.code) throw std::runtime_error(r.err.empty() ? r.out : r.err);
        return r.out;
    }
    void open() {
        selection.select([this](std::vector<std::wstring> args) { return command(std::move(args)); });
        auto state = "," + command({L"shell", L"getprop", L"sys.usb.state"}) + ",";
        if (state.find(",midi,") == std::string::npos)
            throw std::runtime_error("Set phone USB mode to MIDI and enable USB debugging. State: " + state);
        auto value = command({L"forward", L"tcp:0", L"tcp:49155"});
        size_t end = 0;
        int p = std::stoi(value, &end);
        if (end != value.size() || p < 1 || p > 65535) throw std::runtime_error("Invalid ADB forwarding port.");
        port = p;
    }
    ~Forward() {
        if (port) try { command({L"forward", L"--remove", L"tcp:" + std::to_wstring(port)}); } catch (...) {}
    }
};

void readExact(SOCKET s, void* dest, int size) {
    char* out = static_cast<char*>(dest);
    while (size > 0 && !stopping) {
        int n = recv(s, out, size, 0);
        if (n <= 0) throw std::runtime_error("Phone audio disconnected or idle. Start the engine and select PC output.");
        out += n; size -= n;
    }
    if (stopping) throw std::runtime_error("Stopping");
}
void receive(const fs::path& adb, const std::wstring& serial, eray::AudioQueue& queue) {
    std::string lastError;
    while (!stopping) {
        try {
            Forward forward{adb, serial};
            forward.open();
            Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
            if (socket.s == INVALID_SOCKET) throw std::runtime_error("Cannot create audio socket.");
            DWORD timeout = 3000;
            setsockopt(socket.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            sockaddr_in address{}; address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(forward.port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(socket.s, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
                throw std::runtime_error("Cannot connect to the phone audio stream.");
            uint32_t lastRate = 0;
            while (!stopping) {
                uint8_t header[16]; readExact(socket.s, header, sizeof(header));
                auto h = eray::parseHeader(header);
                std::vector<int16_t> samples(h.frames * 2);
                readExact(socket.s, samples.data(), int(samples.size() * 2)); // Windows x64 is little endian.
                queue.push(h.rate, samples);
                if (lastRate != h.rate) {
                    log("USB audio connected: " + std::to_string(h.rate) + " Hz, stereo.");
                    lastRate = h.rate; lastError.clear();
                }
            }
        } catch (const std::exception& e) {
            if (!stopping && lastError != e.what()) {
                lastError = e.what(); log("Waiting: " + lastError);
            }
        }
        queue.clear();
        for (int i = 0; i < 10 && !stopping; ++i) Sleep(100);
    }
}

void play(eray::AudioQueue& queue, int deviceIndex) {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
    struct ComExit { ~ComExit() { CoUninitialize(); } } exit;
    std::string lastError;
    while (!stopping) {
        auto state = queue.state();
        if (!state.rate || !state.ready) { Sleep(10); continue; }
        try {
            Com<IMMDeviceEnumerator> enumerator;
            check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator.p)), "Audio device enumeration");
            Com<IMMDevice> device;
            if (deviceIndex < 0) check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device.p), "Default output device");
            else {
                Com<IMMDeviceCollection> devices;
                check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices.p), "Output devices");
                check(devices->Item(static_cast<UINT>(deviceIndex), &device.p), "Selected output device");
            }
            Com<IAudioClient> client;
            check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client.p)), "Audio activation");
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
            format.nSamplesPerSec = state.rate; format.wBitsPerSample = 16;
            format.nBlockAlign = 4; format.nAvgBytesPerSec = state.rate * 4;
            DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST;
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, &format, nullptr), "WASAPI initialization");
            Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
            if (!event.h) throw std::runtime_error("Cannot create audio event.");
            check(client->SetEventHandle(event.h), "Audio event");
            struct StopAudio { IAudioClient* client; ~StopAudio() { client->Stop(); } } stopAudio{client.p};
            Com<IAudioRenderClient> render;
            check(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render.p)), "Audio renderer");
            Com<IAudioClockAdjustment> clock;
            check(client->GetService(__uuidof(IAudioClockAdjustment), reinterpret_cast<void**>(&clock.p)), "Audio clock adjustment");
            UINT32 capacity = 0;
            check(client->GetBufferSize(&capacity), "Audio buffer size");
            check(client->Start(), "Audio start");
            lastError.clear();
            log("Playing through Windows audio. Press Ctrl+C to stop.");
            double correction = 0;
            while (!stopping) {
                auto current = queue.state();
                if (current.generation != state.generation) break;
                WaitForSingleObject(event.h, 100);
                UINT32 padding = 0;
                check(client->GetCurrentPadding(&padding), "Audio padding");
                UINT32 frames = capacity - std::min(capacity, padding);
                if (!frames) continue;
                BYTE* out = nullptr;
                check(render->GetBuffer(frames, &out), "Audio buffer");
                queue.read(reinterpret_cast<int16_t*>(out), frames, state.generation);
                check(render->ReleaseBuffer(frames, 0), "Audio submit");
                double desired = current.ready ? std::clamp((double(current.frames) / current.target - 1) * .001, -.003, .003) : 0;
                correction += (desired - correction) * .01;
                check(clock->SetSampleRate(float(state.rate * (1 + correction))), "Audio clock correction");
            }
        } catch (const std::exception& e) {
            if (lastError != e.what()) { lastError = e.what(); log("Output: " + lastError); }
            for (int i = 0; i < 10 && !stopping; ++i) Sleep(100);
        }
    }
}

void listDevices() {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
    {
        Com<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator.p)), "Audio enumeration");
        Com<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices.p), "Output devices");
        UINT count = 0; check(devices->GetCount(&count), "Device count");
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> device; check(devices->Item(i, &device.p), "Device");
            Com<IPropertyStore> properties; check(device->OpenPropertyStore(STGM_READ, &properties.p), "Device properties");
            PROPVARIANT value{};
            check(properties->GetValue(PKEY_Device_FriendlyName, &value), "Device name");
            if (value.vt == VT_LPWSTR) log(std::to_string(i) + ": " + utf8(value.pwszVal));
            PropVariantClear(&value);
        }
    }
    CoUninitialize();
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onControl, TRUE);
    SetConsoleTitleW(L"E-ray MIDI USB Audio Receiver");
    int bufferMs = 60, deviceIndex = -1;
    std::wstring serial;
    try {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--list-devices") { listDevices(); return 0; }
            if (arg == L"--help") {
                log("E-rayMIDI-Receiver.exe [--buffer-ms 20..500] [--device INDEX] [--serial USB_SERIAL]\n"
                    "  --list-devices  List Windows output devices\n  --extract-adb   Extract bundled ADB and print its location"); return 0;
            }
            if (arg == L"--extract-adb") { log(utf8(extractAdb().wstring())); return 0; }
            if (++i >= argc) throw std::runtime_error("Missing option value. Use --help.");
            if (arg == L"--buffer-ms") bufferMs = std::stoi(argv[i]);
            else if (arg == L"--device") deviceIndex = std::stoi(argv[i]);
            else if (arg == L"--serial") serial = argv[i];
            else throw std::runtime_error("Unknown option. Use --help.");
        }
        if (bufferMs < 20 || bufferMs > 500 || deviceIndex < -1) throw std::runtime_error("Invalid buffer or device setting.");
        // Avoid two copies fighting for the single phone audio connection.
        Handle instance{CreateMutexW(nullptr, TRUE, L"Local\\E-rayMIDI-Receiver")};
        if (!instance.h || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("E-ray MIDI Receiver is already running.");
        log("E-ray MIDI USB Audio Receiver (Windows x64, USB fix 1)\n"
            "휴대폰에서 USB 디버깅을 허용하고 USB 모드를 MIDI로 설정해 주세요.\n"
            "E-ray MIDI 앱에서 엔진을 시작하고 출력 > PC만 (USB)을 선택해 주세요.\n"
            "Python 설치 없이 실행됩니다. 연결을 기다리는 중입니다...\n");
        fs::path adb = extractAdb();
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed.");
        eray::AudioQueue queue(bufferMs);
        std::thread receiver(receive, adb, serial, std::ref(queue));
        try { play(queue, deviceIndex); }
        catch (...) { stopping = true; receiver.join(); WSACleanup(); throw; }
        stopping = true; receiver.join(); WSACleanup();
        return 0;
    } catch (const std::exception& e) {
        log(std::string("Error: ") + e.what());
        log("Press Enter to close.");
        std::cin.get();
        return 1;
    }
}
