// No-op Discord rich presence, used when the Discord Social SDK is not available.
#include "../../include/services/social.hpp"

#include "spdlog/spdlog.h"

services::Social::Social(const uint64_t application_id, bool) : app_id(application_id), enabled(false) {
    spdlog::info("SOCIAL: built without the Discord Social SDK, rich presence disabled.");
}

services::Social::~Social() = default;

void services::Social::pause() {}
void services::Social::resume() {}
void services::Social::setPosition(const uint64_t) {}
void services::Social::setStatus(const std::string&, const std::string&, const std::string&, const std::string&, uint64_t) {}
void services::Social::removeStatus() {}
void services::Social::threadLoop() {}
void services::Social::updatePresence() {}
