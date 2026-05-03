#pragma once

// CoreMcpBackend — IMcpBackend implementation backed by an emu::Core instance.
//
// Previously "CliMcpBackend", defined inline in cli/main.cpp.
// Now a shared library class usable from both the CLI and the UE5 plugin.
//
// Thread safety: methods are NOT thread-safe against emu::Core.
// The emulator must be paused before calling state-modifying methods
// from a thread other than the one driving emulation.
// The UE5 PSXMcpServerComponent uses this from a dedicated TCP thread —
// safe usage requires calling emu.pause via MCP before modifying state.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "emu/mcp_server.h"
#include "log/emu_log.h"

namespace gpu { enum class PrimOrigin : uint8_t; struct FrameDrawList; }
namespace r3000 { class Cpu; }

namespace emu {

class Core;

// Async log consumer compatible with emu::async_log_init.
// Fills the internal ring buffer (for list_logs) and writes to stderr.
// Pass to async_log_init once at startup to enable MCP log capture.
void core_mcp_log_consumer(uint64_t ts_ns, LogLevel level,
                           const char* tag, const char* msg, void* user);

class CoreMcpBackend final : public IMcpBackend
{
public:
    explicit CoreMcpBackend(Core& core, McpFrontendKind kind = McpFrontendKind::cli);
    ~CoreMcpBackend() override;

    // -- IMcpBackend overrides --
    McpFrontendKind frontend_kind() const override;
    bool get_status(McpStatus& out) const override;
    bool get_cpu_state(McpCpuState& out) const override;
    bool get_run_state(McpRunState& out) const override;
    bool get_boot_exe_info(std::string& out_json, std::string& err) const override;
    bool get_boot_exe_history(std::string& out_json, std::string& err) const override;
    bool get_runtime_module_history(std::string& out_json, std::string& err) const override;
    bool pause(std::string& err) override;
    bool resume(uint32_t max_steps, uint32_t max_frames, bool stop_on_breakpoint,
                std::string& out_json, std::string& err) override;
    bool resume_until_boot_exe(uint32_t max_steps, std::string& out_json, std::string& err) override;
    bool step(uint32_t count, std::string& err) override;
    bool read_ram_u32(uint32_t phys_addr, uint32_t& out, std::string& err) const override;
    bool read_cop0(uint32_t reg, uint32_t& out, std::string& err) const override;
    bool write_cop0(uint32_t reg, uint32_t value, std::string& err) override;
    bool add_step_hook_write_cop0(uint32_t pc, uint32_t reg, uint32_t value, bool once,
                                  uint32_t& hook_id, std::string& err) override;
    bool add_step_hook_write_ram_u32(uint32_t pc, uint32_t phys_addr, uint32_t value, bool once,
                                     uint32_t& hook_id, std::string& err) override;
    bool list_step_hooks(std::string& out_json, std::string& err) const override;
    bool clear_step_hook(uint32_t hook_id, bool& removed, std::string& err) override;
    bool clear_all_step_hooks(uint32_t& removed_count, std::string& err) override;
    bool add_mem_watch_write(uint32_t phys_addr_start, uint32_t phys_addr_end,
                             bool has_pc, uint32_t pc, bool has_value, uint32_t value,
                             bool once, uint32_t& watch_id, std::string& err) override;
    bool list_mem_watches(std::string& out_json, std::string& err) const override;
    bool clear_mem_watch(uint32_t watch_id, bool& removed, std::string& err) override;
    bool clear_all_mem_watches(uint32_t& removed_count, std::string& err) override;
    bool list_mem_watch_events(std::string& out_json, std::string& err) const override;
    bool clear_mem_watch_events(uint32_t& cleared_count, std::string& err) override;
    bool run_until_mem_watch(uint32_t max_steps, uint64_t& event_seq, uint32_t& watch_id,
                             uint32_t& hit_pc, uint32_t& hit_phys_addr, uint32_t& hit_value,
                             uint32_t& hit_size, uint32_t& steps_done, bool& hit,
                             std::string& err) override;
    bool list_logs(uint64_t since_seq, bool has_min_level, uint32_t min_level,
                   const char* tag, const char* contains, uint32_t max_entries,
                   std::string& out_json, std::string& err) const override;
    bool clear_logs(uint32_t& cleared_count, std::string& err) override;
    bool get_gte_trace_summary(std::string& out_json, std::string& err) const override;
    bool get_dma2_nohint_summary(std::string& out_json, std::string& err) const override;
    bool get_draw_list_summary(std::string& out_json, std::string& err) const override;
    bool get_camera_candidates(std::string& out_json, std::string& err) const override;
    bool get_linked_poly_groups(std::string& out_json, std::string& err) const override;
    bool get_transform_roots(std::string& out_json, std::string& err) const override;
    bool get_group_transform_links(std::string& out_json, std::string& err) const override;
    bool get_mesh_cache_candidates(std::string& out_json, std::string& err) const override;
    bool get_pad_state(uint32_t slot, std::string& out_json, std::string& err) const override;
    bool set_pad_state(uint32_t slot, uint16_t buttons_mask, std::string& out_json, std::string& err) override;
    bool tap_pad_buttons(uint32_t slot, uint16_t press_mask, uint32_t hold_steps, uint32_t release_steps,
                         std::string& out_json, std::string& err) override;
    bool tap_pad_named_buttons(uint32_t slot, const char* names_csv, uint32_t hold_steps, uint32_t release_steps,
                               std::string& out_json, std::string& err) override;
    bool hold_pad_buttons(uint32_t slot, uint16_t press_mask, std::string& out_json, std::string& err) override;
    bool hold_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err) override;
    bool release_pad_buttons(uint32_t slot, uint16_t release_mask, std::string& out_json, std::string& err) override;
    bool release_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err) override;
    bool get_scene_vector_snapshot(uint32_t max_groups, uint32_t max_roots,
                                   bool include_hud, bool include_raw_triangles,
                                   std::string& out_json, std::string& err) const override;
    bool get_scene_delta(uint32_t max_groups, std::string& out_json, std::string& err) override;
    bool get_object_candidates(uint32_t max_objects, std::string& out_json, std::string& err) const override;
    bool get_scene_salience_summary(uint32_t max_targets, std::string& out_json, std::string& err) const override;
    bool get_hierarchy_candidates(uint32_t max_nodes, std::string& out_json, std::string& err) const override;
    bool get_focus_candidate(std::string& out_json, std::string& err) const override;
    bool step_with_pad_observation(uint32_t slot, const char* names_csv, uint32_t hold_steps,
                                   uint32_t observe_steps, uint32_t max_groups,
                                   uint32_t max_targets, std::string& out_json,
                                   std::string& err) override;
    bool match_render_pattern(uint32_t max_candidates, std::string& out_json,
                              std::string& err) const override;
    bool set_psx3d_mode(const char* mode, std::string& err) override;
    bool request_psx3d_refresh(const char* reason, const char* scope,
                               uint32_t& id, std::string& err) override;
    bool set_gte_trace_window(uint32_t pc_start, uint32_t pc_end,
                              uint32_t start_frame, uint32_t end_frame,
                              bool enabled, std::string& err) override;
    bool list_breakpoints(std::string& out_json, std::string& err) const override;
    bool set_breakpoint_pc(uint32_t pc, std::string& err) override;
    bool clear_breakpoint_pc(uint32_t pc, bool& removed, std::string& err) override;
    bool clear_all_breakpoints(uint32_t& removed_count, std::string& err) override;
    bool run_until_breakpoint(uint32_t max_steps, uint32_t& hit_pc,
                              uint32_t& steps_done, bool& hit,
                              std::string& err) override;

private:
    // -----------------------------------------------------------------------
    // Inner types (all defined here since they are used in method signatures
    // or as data member types below)
    // -----------------------------------------------------------------------

    struct GroupObs
    {
        uint32_t source_pc{0};
        uint32_t face_idx{0xFFFFFFFFu};
        uint32_t tri_count{0};
        uint32_t quad_halves{0};
        uint32_t min_ot_z{0xFFFFFFFFu};
        uint32_t max_ot_z{0};
        uint32_t textured{0};
        uint32_t semi{0};
        uint32_t raw{0};
        int16_t min_x{32767}, min_y{32767};
        int16_t max_x{-32768}, max_y{-32768};
        uint32_t first_frame{0};
        uint32_t last_frame{0};
        uint32_t seen_frames{0};
        uint32_t stable_score{0};
    };

    struct GroupTemporalState
    {
        uint32_t source_pc{0};
        uint32_t face_idx{0xFFFFFFFFu};
        uint32_t first_frame{0};
        uint32_t last_frame{0};
        uint32_t seen_frames{0};
        uint32_t last_tri_count{0};
        uint32_t stable_hits{0};
        uint32_t last_min_ot_z{0};
        uint32_t last_max_ot_z{0};
        bool last_valid{false};
    };

    struct MeshCacheCandidate
    {
        uint32_t source_pc{0};
        uint32_t face_idx{0xFFFFFFFFu};
        uint32_t first_frame{0};
        uint32_t last_frame{0};
        uint32_t seen_frames{0};
        uint32_t last_tri_count{0};
        bool stable_ot_z{false};
        std::string material_signature{};
        uint32_t promotion_score{0};
        std::string status{};
    };

    struct SceneGroupSnapshot
    {
        std::string group_id{};
        uint32_t source_pc{0};
        uint32_t face_idx{0xFFFFFFFFu};
        uint32_t tri_count{0};
        uint32_t quad_halves{0};
        uint32_t min_ot_z{0};
        uint32_t max_ot_z{0};
        uint32_t textured{0};
        uint32_t semi{0};
        uint32_t raw{0};
        uint32_t seen_frames{0};
        uint32_t stable_score{0};
        uint32_t promotion_score{0};
        int16_t min_x{0}, min_y{0}, max_x{0}, max_y{0};
        float center_x{0.0f}, center_y{0.0f};
        uint32_t extent_w{0}, extent_h{0};
        uint32_t root_addr{0};
        uint32_t root_last_pc{0};
        uint32_t root_score{0};
        float root_confidence{0.0f};
        std::string material_signature{};
        std::string kind{};
        std::string notes{};
    };

    struct SceneRootSnapshot
    {
        uint32_t addr{0};
        std::string region{};
        uint32_t score{0};
        uint32_t hits{0};
        uint32_t frame_hits{0};
        uint32_t last_pc{0};
        uint32_t gte_reg_mask{0};
    };

    struct SceneRawTriangleSnapshot
    {
        uint32_t index{0};
        std::string origin{};
        uint32_t source_pc{0};
        uint32_t face_idx{0xFFFFFFFFu};
        uint32_t ot_z{0};
        bool textured{false};
        bool semi{false};
        bool raw{false};
        int16_t x[3]{};
        int16_t y[3]{};
        uint8_t r[3]{};
        uint8_t g[3]{};
        uint8_t b[3]{};
        int16_t min_x{0}, min_y{0}, max_x{0}, max_y{0};
        uint8_t avg_r{0}, avg_g{0}, avg_b{0};
    };

    struct SceneRawRectSnapshot
    {
        uint32_t first_triangle_index{0};
        std::string origin{};
        int16_t min_x{0}, min_y{0}, max_x{0}, max_y{0};
        float center_x{0.0f}, center_y{0.0f};
        uint32_t width{0}, height{0};
        uint8_t avg_r{0}, avg_g{0}, avg_b{0};
    };

    struct SceneVectorSnapshot
    {
        uint32_t frame_id{0};
        uint32_t display_width{0};
        uint32_t display_height{0};
        bool pal{false};
        bool hud_present{false};
        uint32_t dominant_source_pc{0};
        uint32_t dominant_root_addr{0};
        uint32_t dominant_group_count{0};
        float confidence{0.0f};
        std::string dominant_kind{};
        std::string summary{};
        std::vector<SceneGroupSnapshot> groups{};
        std::vector<SceneRootSnapshot> roots{};
        std::vector<SceneRawTriangleSnapshot> raw_triangles{};
        std::vector<SceneRawRectSnapshot> raw_rects{};
    };

    struct ObservationTimingState
    {
        bool valid{false};
        uint32_t last_frame_id{0};
        std::chrono::steady_clock::time_point last_tp{};
    };

    enum class StepHookKind : uint32_t
    {
        write_cop0 = 0,
        write_ram_u32 = 1,
    };

    struct StepHookRule
    {
        uint32_t id{0};
        uint32_t pc{0};
        StepHookKind kind{StepHookKind::write_cop0};
        uint32_t arg0{0};
        uint32_t arg1{0};
        bool once{true};
        bool enabled{true};
        uint64_t hit_count{0};
    };

    struct MemWatchRule
    {
        uint32_t id{0};
        uint32_t phys_addr_start{0};
        uint32_t phys_addr_end{0};
        bool has_pc{false};
        uint32_t pc{0};
        bool has_value{false};
        uint32_t value{0};
        bool once{false};
        bool enabled{true};
        uint64_t hit_count{0};
    };

    struct MemWatchEvent
    {
        uint64_t seq{0};
        uint32_t watch_id{0};
        uint32_t pc{0};
        uint32_t phys_addr{0};
        uint32_t value{0};
        uint32_t size{0};
    };

    // -----------------------------------------------------------------------
    // Private methods
    // -----------------------------------------------------------------------

    static void step_hook_trampoline(uint32_t pc, void* user);
    static void write_hook_trampoline(uint32_t phys_addr, uint32_t value,
                                      uint32_t size, void* user);
    void on_step_hook(uint32_t pc);
    void on_write_hook(uint32_t phys_addr, uint32_t value, uint32_t size);

    static std::vector<SceneRootSnapshot> build_sorted_scene_roots(
        const Core& core, const r3000::Cpu& cpu, uint32_t max_roots);
    bool build_sorted_group_obs(gpu::FrameDrawList& dl,
                                std::vector<GroupObs>& sorted,
                                std::string& err) const;
    void update_temporal_group_cache(const gpu::FrameDrawList& dl) const;
    bool build_scene_vector_snapshot(uint32_t max_groups, uint32_t max_roots,
                                     bool include_hud, bool include_raw_triangles,
                                     SceneVectorSnapshot& out, std::string& err) const;
    std::string make_observation_timing_json(uint32_t frame_id) const;
    static std::string add_timing_to_json_object(const std::string& base_json,
                                                  const std::string& timing_json);
    static const char* prim_origin_name(gpu::PrimOrigin origin);
    static std::string scene_vector_snapshot_to_json(const SceneVectorSnapshot& s);
    static std::string scene_delta_to_json(const SceneVectorSnapshot& prev,
                                           const SceneVectorSnapshot& current, bool has_prev);
    static std::string object_candidates_to_json(const SceneVectorSnapshot& s, uint32_t max_objects);
    static std::string scene_salience_to_json(const SceneVectorSnapshot& s, uint32_t max_targets);
    static std::string focus_candidate_to_json(const SceneVectorSnapshot& s);
    static std::string hierarchy_candidates_to_json(const SceneVectorSnapshot& s, uint32_t max_nodes);

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    Core& core_;
    McpFrontendKind kind_;
    int step_hook_handle_{-1};
    int write_hook_handle_{-1};
    McpRunState run_state_{};
    std::vector<McpBreakpoint> breakpoints_{};
    std::vector<StepHookRule> step_hooks_{};
    uint32_t next_step_hook_id_{1};

    std::vector<MemWatchRule> mem_watches_{};
    static constexpr uint32_t kMaxMemWatchEvents = 256;
    std::array<MemWatchEvent, kMaxMemWatchEvents> mem_watch_events_{};
    uint32_t mem_watch_event_head_{0};
    uint32_t mem_watch_event_count_{0};
    uint32_t next_mem_watch_id_{1};
    uint64_t next_mem_watch_event_seq_{1};
    MemWatchEvent last_mem_watch_event_{};
    uint64_t last_mem_watch_event_seq_{0};

    mutable uint32_t temporal_last_frame_id_{0xFFFFFFFFu};
    mutable std::unordered_map<uint64_t, GroupTemporalState> temporal_groups_{};
    mutable std::unordered_map<uint64_t, MeshCacheCandidate> mesh_cache_candidates_{};
    mutable bool has_last_scene_snapshot_{false};
    mutable SceneVectorSnapshot last_scene_snapshot_{};
    mutable ObservationTimingState observation_timing_state_{};
};

} // namespace emu
