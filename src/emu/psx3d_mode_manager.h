#pragma once

#include <cstdint>
#include <string>

namespace emu
{

enum class Psx3dRunMode : uint8_t
{
    game = 0,
    analysis = 1,
};

struct Psx3dRefreshRequest
{
    uint32_t id{0};
    std::string reason{};
    std::string scope{};
};

// Runtime policy for PSX3D analysis/profile usage.
// This class does not perform analysis itself; it only controls when analysis is allowed
// and when a refresh has been requested.
class Psx3dModeManager
{
  public:
    void reset();

    void set_mode(Psx3dRunMode m) { mode_ = m; }
    Psx3dRunMode mode() const { return mode_; }

    void set_analysis_enabled(bool v) { analysis_enabled_ = v; }
    bool analysis_enabled() const { return analysis_enabled_; }

    bool analysis_active() const { return analysis_enabled_ && mode_ == Psx3dRunMode::analysis; }

    // Queue or replace a refresh request. Returns request id.
    uint32_t request_refresh(const std::string& reason, const std::string& scope);
    bool has_refresh_request() const { return refresh_pending_; }
    Psx3dRefreshRequest consume_refresh_request();

  private:
    Psx3dRunMode mode_{Psx3dRunMode::game};
    bool analysis_enabled_{false};

    uint32_t next_request_id_{1};
    bool refresh_pending_{false};
    Psx3dRefreshRequest pending_{};
};

} // namespace emu

