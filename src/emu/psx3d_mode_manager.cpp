#include "psx3d_mode_manager.h"

namespace emu
{

void Psx3dModeManager::reset()
{
    mode_ = Psx3dRunMode::game;
    analysis_enabled_ = false;
    next_request_id_ = 1;
    refresh_pending_ = false;
    pending_ = {};
}

uint32_t Psx3dModeManager::request_refresh(const std::string& reason, const std::string& scope)
{
    Psx3dRefreshRequest req{};
    req.id = next_request_id_++;
    req.reason = reason;
    req.scope = scope;
    pending_ = req;
    refresh_pending_ = true;
    return req.id;
}

Psx3dRefreshRequest Psx3dModeManager::consume_refresh_request()
{
    if (!refresh_pending_)
        return {};
    refresh_pending_ = false;
    return pending_;
}

} // namespace emu

