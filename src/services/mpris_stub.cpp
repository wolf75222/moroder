// No-op MPRIS backend, used where D-Bus is not available (e.g. macOS).
#include "../../include/services/mpris.hpp"

using namespace services;

std::unique_ptr<Mpris> Mpris::make(std::string_view name) { return std::make_unique<Mpris>(name); }
Mpris::Mpris(std::string_view player_name) : service_name(PREFIX + std::string(player_name)) {}

void Mpris::startLoopAsync() {}
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
