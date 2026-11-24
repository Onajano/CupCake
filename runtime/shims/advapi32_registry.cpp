// advapi32.dll's registry functions, for the one key the title reads: the console type.
//
// The title asks HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\OEM for the REG_DWORD ConsoleMode and
// maps it to an XSystemDeviceType itself (RegOpenKeyW, RegQueryValueExW and RegCloseKey are its only advapi32 imports;
// measured in the retail executable's console-type query): 0x60 Series S (Lockhart), 0x64 Series X (Anaconda),
// 0x68 the Series devkit, 0x58, 0x50 and 0x40 the Xbox One X devkit, One X and One S, 0x10 the Xbox One, any other
// value Unknown, and a missing key or value Pc. A PC has no such key, so without this the title saw Pc while
// XSystemGetDeviceType said Series X. The key is answered from XSystemGetDeviceType (LEONIDA_DEVICE_TYPE, default
// Series X), so the two always agree. Every other key goes to the host's registry: advapi32.dll is a host DLL of
// which only these three names are replaced (registry.cpp, owns_dll).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

#include "shims.h"

#pragma comment(lib, "advapi32.lib")

namespace rt::shims {
namespace {

constexpr wchar_t kOemPath[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr wchar_t kConsoleMode[] = L"ConsoleMode";

// The handle given for the OEM key. Registry handles are kernel handles (multiples of 4) or the predefined HKEY_*
// values (0x80000000 and up), so an odd value below them is never a real one.
const HKEY kOemKey = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x4f454d1u));

uint32_t device_type() {
    using Fn = uint32_t(__stdcall*)();
    if (const auto fn = reinterpret_cast<Fn>(find("xgameruntime.dll", "XSystemGetDeviceType"))) return fn();
    return 7;  // XboxScarlettAnaconda, XSystemGetDeviceType's default
}

// ConsoleMode for the current device type, in the title's mapping above; false for Pc (no key).
bool console_mode(DWORD& value) {
    static constexpr DWORD kByType[] = {0xffff, 0, 0x10, 0x40, 0x50, 0x58, 0x60, 0x64, 0x68};
    const uint32_t type = device_type();
    if (type == 1 || type >= std::size(kByType)) return false;
    value = kByType[type];
    return true;
}

LSTATUS WINAPI OpenKeyW(HKEY root, LPCWSTR sub_key, PHKEY out) {
    if (root != HKEY_LOCAL_MACHINE || !sub_key || _wcsicmp(sub_key, kOemPath) != 0)
        return RegOpenKeyW(root, sub_key, out);
    if (!out) return ERROR_INVALID_PARAMETER;
    DWORD value = 0;
    if (!console_mode(value)) return ERROR_FILE_NOT_FOUND;
    *out = kOemKey;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI QueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (key != kOemKey) return RegQueryValueExW(key, name, reserved, type, data, size);
    if (reserved || (data && !size)) return ERROR_INVALID_PARAMETER;
    DWORD value = 0;
    if (!name || _wcsicmp(name, kConsoleMode) != 0 || !console_mode(value)) return ERROR_FILE_NOT_FOUND;
    if (type) *type = REG_DWORD;
    if (data) {
        if (*size < sizeof(value)) {
            *size = sizeof(value);
            return ERROR_MORE_DATA;
        }
        std::memcpy(data, &value, sizeof(value));
    }
    if (size) *size = sizeof(value);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI CloseKey(HKEY key) { return key == kOemKey ? ERROR_SUCCESS : RegCloseKey(key); }

const Entry kEntries[] = {
    {"advapi32.dll", "RegOpenKeyW", reinterpret_cast<void*>(&OpenKeyW)},
    {"advapi32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&QueryValueExW)},
    {"advapi32.dll", "RegCloseKey", reinterpret_cast<void*>(&CloseKey)},
};
const Table kTable(kEntries, std::size(kEntries));

}  // namespace
}  // namespace rt::shims
