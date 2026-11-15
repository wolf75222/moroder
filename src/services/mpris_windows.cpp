/* WINDOWS: NOW PLAYING AND MEDIA KEYS
- The MPRIS interface on Windows, through the System Media Transport
  Controls (SMTC): the track (title, artist, album, cover) in Windows' media
  flyout and on the lock screen, its play / pause / next / previous buttons,
  and the keyboard's media keys, which Windows routes there
- Raw WinRT (the ABI interfaces MinGW ships, RoGetActivationFactory and
  ISystemMediaTransportControlsInterop::GetForWindow on a hidden window of
  ours), no C++/WinRT
- If the SMTC can't start, a low-level keyboard hook still catches the media
  keys
*/
#include "../../include/services/mpris.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <initguid.h>          // the WinRT IIDs below are defined in this file
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <systemmediatransportcontrolsinterop.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace WF = ABI::Windows::Foundation;
namespace WM = ABI::Windows::Media;
namespace WSS = ABI::Windows::Storage::Streams;

namespace {

std::atomic<services::Mpris*> instance{nullptr};

// IAgileObject: the handler can be called from any thread (not in every MinGW header set).
const GUID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct HStr {
    HSTRING h = nullptr;
    explicit HStr(const std::wstring& s) { WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h); }
    ~HStr() { if (h) WindowsDeleteString(h); }
};

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// SMTC
// ---------------------------------------------------------------------------

std::mutex smtc_mutex;
WM::ISystemMediaTransportControls* smtc = nullptr;
WM::ISystemMediaTransportControlsDisplayUpdater* updater = nullptr;

using ButtonHandlerBase = WF::ITypedEventHandler<WM::SystemMediaTransportControls*, WM::SystemMediaTransportControlsButtonPressedEventArgs*>;

// Called by Windows (any thread) when a flyout button or a media key is pressed.
class ButtonHandler : public ButtonHandlerBase {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_Agile) ||
            IsEqualIID(iid, IID___FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CSystemMediaTransportControlsButtonPressedEventArgs)) {
            *out = static_cast<ButtonHandlerBase*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Invoke(WM::ISystemMediaTransportControls*, WM::ISystemMediaTransportControlsButtonPressedEventArgs* args) override {
        WM::SystemMediaTransportControlsButton button;
        if (!args || FAILED(args->get_Button(&button))) return S_OK;
        if (services::Mpris* m = instance.load()) {
            const int code = static_cast<int>(button);
            std::thread([m, code] { m->smtcButton(code); }).detach();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

HWND hiddenWindow() {
    static const wchar_t* cls = L"MoroderMediaWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    // A top-level window (SMTC wants one), never shown.
    return CreateWindowExW(0, cls, L"Moroder", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
}

bool startSmtc(HWND window) {
    ISystemMediaTransportControlsInterop* interop = nullptr;
    HStr cls(RuntimeClass_Windows_Media_SystemMediaTransportControls);
    if (FAILED(RoGetActivationFactory(cls.h, IID_ISystemMediaTransportControlsInterop, reinterpret_cast<void**>(&interop)))) return false;
    WM::ISystemMediaTransportControls* controls = nullptr;
    const HRESULT hr = interop->GetForWindow(window, IID___x_ABI_CWindows_CMedia_CISystemMediaTransportControls, reinterpret_cast<void**>(&controls));
    interop->Release();
    if (FAILED(hr) || !controls) return false;

    controls->put_IsEnabled(true);
    controls->put_IsPlayEnabled(true);
    controls->put_IsPauseEnabled(true);
    controls->put_IsNextEnabled(true);
    controls->put_IsPreviousEnabled(true);
    controls->put_IsStopEnabled(true);
    controls->put_PlaybackStatus(WM::MediaPlaybackStatus_Stopped);

    auto* handler = new ButtonHandler();
    EventRegistrationToken token{};
    controls->add_ButtonPressed(handler, &token);
    handler->Release();      // the controls hold it now

    WM::ISystemMediaTransportControlsDisplayUpdater* du = nullptr;
    controls->get_DisplayUpdater(&du);

    std::lock_guard lock(smtc_mutex);
    smtc = controls;
    updater = du;
    return true;
}

// A cover from its URL (https://, or file:// for a local one), "" leaves none.
WSS::IRandomAccessStreamReference* thumbnailFrom(const std::string& url) {
    if (url.empty()) return nullptr;
    WF::IUriRuntimeClassFactory* uris = nullptr;
    HStr uri_cls(RuntimeClass_Windows_Foundation_Uri);
    if (FAILED(RoGetActivationFactory(uri_cls.h, IID___x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory, reinterpret_cast<void**>(&uris)))) return nullptr;
    WF::IUriRuntimeClass* uri = nullptr;
    HStr text(wide(url.rfind("http", 0) == 0 || url.rfind("file:", 0) == 0 ? url : "file:///" + url));
    uris->CreateUri(text.h, &uri);
    uris->Release();
    if (!uri) return nullptr;

    WSS::IRandomAccessStreamReferenceStatics* refs = nullptr;
    HStr ref_cls(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference);
    WSS::IRandomAccessStreamReference* ref = nullptr;
    if (SUCCEEDED(RoGetActivationFactory(ref_cls.h, IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamReferenceStatics, reinterpret_cast<void**>(&refs)))) {
        refs->CreateFromUri(uri, &ref);
        refs->Release();
    }
    uri->Release();
    return ref;
}

// ---------------------------------------------------------------------------
// fallback: media keys only
// ---------------------------------------------------------------------------

LRESULT CALLBACK onKey(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        if (services::Mpris* m = instance.load()) {
            // Run off the hook: it must return quickly or Windows drops it.
            const DWORD vk = k->vkCode;
            if (vk == VK_MEDIA_PLAY_PAUSE || vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK || vk == VK_MEDIA_STOP)
                std::thread([m, vk] { m->mediaKey(static_cast<int>(vk)); }).detach();
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

const sdbus::Variant* field(const std::map<services::Field, sdbus::Variant>& m, services::Field f) {
    auto it = m.find(f);
    return it == m.end() ? nullptr : &it->second;
}

std::string text(const std::map<services::Field, sdbus::Variant>& m, services::Field f) {
    const sdbus::Variant* v = field(m, f);
    if (!v) return "";
    if (auto s = std::get_if<std::string>(&v->get())) return *s;
    return "";
}

}

using namespace services;

std::unique_ptr<Mpris> Mpris::make(std::string_view name) { return std::make_unique<Mpris>(name); }
Mpris::Mpris(std::string_view player_name) : service_name(PREFIX + std::string(player_name)) {}

void Mpris::startLoopAsync() {
    if (instance.exchange(this) == this) return;
    std::thread([] {
        RoInitialize(RO_INIT_MULTITHREADED);
        HWND window = hiddenWindow();
        // SMTC, else just the media keys through a keyboard hook (not both:
        // Windows routes the keys to the SMTC, a hook would see them twice).
        HHOOK hook = nullptr;
        if (!window || !startSmtc(window)) hook = SetWindowsHookExW(WH_KEYBOARD_LL, onKey, GetModuleHandleW(nullptr), 0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (hook) UnhookWindowsHookEx(hook);
    }).detach();
}

void Mpris::smtcButton(int button) {
    switch (static_cast<WM::SystemMediaTransportControlsButton>(button)) {
        case WM::SystemMediaTransportControlsButton_Play:     if (play_fn) play_fn(); else if (play_pause_fn) play_pause_fn(); break;
        case WM::SystemMediaTransportControlsButton_Pause:    if (pause_fn) pause_fn(); else if (play_pause_fn) play_pause_fn(); break;
        case WM::SystemMediaTransportControlsButton_Next:     if (next_fn) next_fn(); break;
        case WM::SystemMediaTransportControlsButton_Previous: if (previous_fn) previous_fn(); break;
        case WM::SystemMediaTransportControlsButton_Stop:     if (stop_fn) stop_fn(); break;
        default: break;
    }
}

void Mpris::mediaKey(int vk) {
    switch (vk) {
        case VK_MEDIA_PLAY_PAUSE: if (play_pause_fn) play_pause_fn(); break;
        case VK_MEDIA_NEXT_TRACK: if (next_fn) next_fn(); break;
        case VK_MEDIA_PREV_TRACK: if (previous_fn) previous_fn(); break;
        case VK_MEDIA_STOP:       if (stop_fn) stop_fn(); break;
        default: break;
    }
}
void Mpris::startLoop() {}

void Mpris::setMetadata(const std::map<Field, sdbus::Variant>& value) {
    std::lock_guard lock(smtc_mutex);
    if (!updater) return;
    updater->ClearAll();
    updater->put_Type(WM::MediaPlaybackType_Music);
    WM::IMusicDisplayProperties* music = nullptr;
    if (SUCCEEDED(updater->get_MusicProperties(&music)) && music) {
        HStr title(wide(text(value, Field::Title))), artist(wide(text(value, Field::Artist)));
        music->put_Title(title.h);
        music->put_Artist(artist.h);
        WM::IMusicDisplayProperties2* music2 = nullptr;
        if (SUCCEEDED(music->QueryInterface(IID___x_ABI_CWindows_CMedia_CIMusicDisplayProperties2, reinterpret_cast<void**>(&music2))) && music2) {
            HStr album(wide(text(value, Field::Album)));
            music2->put_AlbumTitle(album.h);
            music2->Release();
        }
        music->Release();
    }
    if (WSS::IRandomAccessStreamReference* cover = thumbnailFrom(text(value, Field::ArtUrl))) {
        updater->put_Thumbnail(cover);
        cover->Release();
    }
    updater->Update();
}
void Mpris::setPlaybackStatus(PlaybackStatus value) {
    playback_status = value;
    std::lock_guard lock(smtc_mutex);
    if (!smtc) return;
    smtc->put_PlaybackStatus(value == PlaybackStatus::Playing ? WM::MediaPlaybackStatus_Playing
                           : value == PlaybackStatus::Paused  ? WM::MediaPlaybackStatus_Paused
                                                              : WM::MediaPlaybackStatus_Stopped);
}

void Mpris::onLoopStatusChanged(std::function<void(LoopStatus)> fn) { loop_status_changed_fn = std::move(fn); }
void Mpris::onShuffleChanged(std::function<void(bool)> fn) { shuffle_changed_fn = std::move(fn); }

void Mpris::onPrevious(std::function<void(void)> fn) { previous_fn = std::move(fn); }
void Mpris::onToggle(std::function<void(void)> fn) { play_pause_fn = std::move(fn); }
void Mpris::onPause(std::function<void(void)> fn) { pause_fn = std::move(fn); }
void Mpris::onNext(std::function<void(void)> fn) { next_fn = std::move(fn); }
void Mpris::onStop(std::function<void(void)> fn) { stop_fn = std::move(fn); }
void Mpris::onPlay(std::function<void(void)> fn) { play_fn = std::move(fn); }
void Mpris::onQuit(std::function<void(void)> fn) { quit_fn = std::move(fn); }

void Mpris::onSetPosition(std::function<void(int64_t)> fn) { setPosition_fn = std::move(fn); }
void Mpris::onSeek(std::function<void(int64_t)> fn) { seek_fn = std::move(fn); }

void Mpris::setHumanName(std::string_view value) { human_name = value; }
void Mpris::setLoopStatus(LoopStatus value) { loop_status = value; }
void Mpris::setPosition(int64_t value) { position = value; }
void Mpris::setShuffle(bool value) { shuffle = value; }

void Mpris::sendSeekedSignal(int64_t) {}

void Mpris::setIsNextPossible(bool possible) { is_next_possible = possible; }
void Mpris::setIsPreviousPossible(bool possible) { is_previous_possible = possible; }
void Mpris::updatePlayerControls() {}
