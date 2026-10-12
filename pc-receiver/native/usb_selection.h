#pragma once
#include <string>
#include <vector>
#include <stdexcept>

namespace eray {
// Windows USB transports need not have a devpath. Every command uses ADB's
// USB selector, including reconnect and cleanup; never fall back to -s.
struct UsbSelection {
    std::wstring requestedSerial;
    std::vector<std::wstring> selector() const { return {L"-d"}; }
    template<class Command> void select(Command command) {
        auto serial = command(std::vector<std::wstring>{L"get-serialno"});
        if (serial.empty() || serial == "unknown")
            throw std::runtime_error("No authorized USB phone. Connect one phone and allow USB debugging.");
        if (!requestedSerial.empty() && std::wstring(serial.begin(), serial.end()) != requestedSerial)
            throw std::runtime_error("USB phone serial does not match --serial. Connect only the requested USB phone.");
    }
};
}
