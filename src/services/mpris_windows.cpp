/* WINDOWS MEDIA KEYS
- The MPRIS interface on Windows: play / pause, next, previous and stop from
  the keyboard's media keys (and headsets that send them), wherever the focus
  is. A low-level keyboard hook on a thread of its own sees them system-wide
  without taking them from other players; the rest of the interface is the
  no-op one (no D-Bus here)
*/
#include "../../include/services/mpris.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <thread>

namespace {

std::atomic<services::Mpris*> instance{nullptr};

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

}

using namespace services;

std::unique_ptr<Mpris> Mpris::make(std::string_view name) { return std::make_unique<Mpris>(name); }
Mpris::Mpris(std::string_view player_name) : service_name(PREFIX + std::string(player_name)) {}

void Mpris::startLoopAsync() {
    if (instance.exchange(this) == this) return;
    std::thread([] {
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, onKey, GetModuleHandleW(nullptr), 0);
        if (!hook) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        UnhookWindowsHookEx(hook);
    }).detach();
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

void Mpris::setMetadata(const std::map<Field, sdbus::Variant> &) {}
void Mpris::setPlaybackStatus(PlaybackStatus value) { playback_status = value; }

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
