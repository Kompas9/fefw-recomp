// SPDX-License-Identifier: GPL-3.0-or-later
// Lists the active render endpoints (IMMDeviceEnumerator) for [audio] device and for the start-up log of the XAudio2 sink.
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>
#include <vector>

#include "nxrt/audio_sink.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")       // PKEY_Device_FriendlyName

namespace nxrt::audio {
namespace {

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

std::wstring default_output_id() {
    std::wstring out;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com != S_OK && com != S_FALSE && com != RPC_E_CHANGED_MODE) return out;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) &&
        en) {
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) {
                out = id;
                CoTaskMemFree(id);
            }
            def->Release();
        }
        en->Release();
    }
    if (com == S_OK || com == S_FALSE) CoUninitialize();
    return out;
}

std::vector<OutputDevice> list_output_devices() {
    std::vector<OutputDevice> out;
    // Balanced with CoUninitialize below. An existing COM mode on this thread (RPC_E_CHANGED_MODE) is kept.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com != S_OK && com != S_FALSE && com != RPC_E_CHANGED_MODE) return out;
    const bool owns_com = com == S_OK || com == S_FALSE;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) &&
        en) {
        std::wstring default_id;
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) {
                default_id = id;
                CoTaskMemFree(id);
            }
            def->Release();
        }

        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || !dev) continue;

                OutputDevice d;
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id) {
                    d.id = id;
                    CoTaskMemFree(id);
                }
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                        d.name = wide_to_utf8(pv.pwszVal);
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                d.is_default = !d.id.empty() && d.id == default_id;
                out.push_back(std::move(d));
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }

    if (owns_com) CoUninitialize();
    return out;
}

}  // namespace nxrt::audio
