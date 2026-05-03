#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace emu
{

class IMcpCallGuard
{
public:
    virtual ~IMcpCallGuard() = default;
    virtual bool before_tool_call(const char* tool_name, std::string& err) = 0;
    virtual void after_tool_call(const char* tool_name, bool succeeded) = 0;
};

enum class McpFrontendKind : uint32_t
{
    cli = 0,
    ue5 = 1,
};

struct McpCpuState
{
    uint32_t pc = 0;
    uint32_t hi = 0;
    uint32_t lo = 0;
    uint32_t gpr[32]{};
};

struct McpStatus
{
    bool has_core = false;
    bool has_cpu = false;
    bool analysis_enabled = false;
    bool analysis_mode = false;
    bool profile_override = false;
    uint32_t frame_count = 0;
    uint32_t tri_3d = 0;
    uint32_t tri_2d = 0;
    std::string profile_path{};
    std::string profile_game_id{};
};

struct McpBreakpoint
{
    uint32_t pc = 0;
    bool enabled = false;
    uint64_t hit_count = 0;
};

struct McpRunState
{
    bool paused = true;
    uint32_t last_pc = 0;
    uint32_t last_steps = 0;
    uint32_t last_frames = 0;
    bool last_hit_breakpoint = false;
    uint32_t last_hit_pc = 0;
};

class IMcpBackend
{
public:
    virtual ~IMcpBackend() = default;

    virtual McpFrontendKind frontend_kind() const = 0;
    virtual bool get_status(McpStatus& out) const = 0;
    virtual bool get_cpu_state(McpCpuState& out) const = 0;
    virtual bool get_run_state(McpRunState& out) const = 0;
    virtual bool get_boot_exe_info(std::string& out_json, std::string& err) const = 0;
    virtual bool get_boot_exe_history(std::string& out_json, std::string& err) const = 0;
    virtual bool get_runtime_module_history(std::string& out_json, std::string& err) const = 0;
    virtual bool pause(std::string& err) = 0;
    virtual bool resume(uint32_t max_steps, uint32_t max_frames, bool stop_on_breakpoint,
        std::string& out_json, std::string& err) = 0;
    virtual bool resume_until_boot_exe(uint32_t max_steps, std::string& out_json, std::string& err) = 0;
    virtual bool step(uint32_t count, std::string& err) = 0;
    virtual bool read_ram_u32(uint32_t phys_addr, uint32_t& out, std::string& err) const = 0;
    virtual bool read_cop0(uint32_t reg, uint32_t& out, std::string& err) const = 0;
    virtual bool write_cop0(uint32_t reg, uint32_t value, std::string& err) = 0;
    virtual bool add_step_hook_write_cop0(uint32_t pc, uint32_t reg, uint32_t value, bool once, uint32_t& hook_id, std::string& err) = 0;
    virtual bool add_step_hook_write_ram_u32(uint32_t pc, uint32_t phys_addr, uint32_t value, bool once, uint32_t& hook_id, std::string& err) = 0;
    virtual bool list_step_hooks(std::string& out_json, std::string& err) const = 0;
    virtual bool clear_step_hook(uint32_t hook_id, bool& removed, std::string& err) = 0;
    virtual bool clear_all_step_hooks(uint32_t& removed_count, std::string& err) = 0;
    virtual bool add_mem_watch_write(uint32_t phys_addr_start, uint32_t phys_addr_end, bool has_pc, uint32_t pc,
        bool has_value, uint32_t value, bool once, uint32_t& watch_id, std::string& err) = 0;
    virtual bool list_mem_watches(std::string& out_json, std::string& err) const = 0;
    virtual bool clear_mem_watch(uint32_t watch_id, bool& removed, std::string& err) = 0;
    virtual bool clear_all_mem_watches(uint32_t& removed_count, std::string& err) = 0;
    virtual bool list_mem_watch_events(std::string& out_json, std::string& err) const = 0;
    virtual bool clear_mem_watch_events(uint32_t& cleared_count, std::string& err) = 0;
    virtual bool run_until_mem_watch(uint32_t max_steps, uint64_t& event_seq, uint32_t& watch_id,
        uint32_t& hit_pc, uint32_t& hit_phys_addr, uint32_t& hit_value, uint32_t& hit_size,
        uint32_t& steps_done, bool& hit, std::string& err) = 0;
    virtual bool list_logs(uint64_t since_seq, bool has_min_level, uint32_t min_level,
        const char* tag, const char* contains, uint32_t max_entries, std::string& out_json, std::string& err) const = 0;
    virtual bool clear_logs(uint32_t& cleared_count, std::string& err) = 0;
    virtual bool get_gte_trace_summary(std::string& out_json, std::string& err) const = 0;
    virtual bool get_dma2_nohint_summary(std::string& out_json, std::string& err) const = 0;
    virtual bool get_draw_list_summary(std::string& out_json, std::string& err) const = 0;
    virtual bool get_camera_candidates(std::string& out_json, std::string& err) const = 0;
    virtual bool get_linked_poly_groups(std::string& out_json, std::string& err) const = 0;
    virtual bool get_transform_roots(std::string& out_json, std::string& err) const = 0;
    virtual bool get_group_transform_links(std::string& out_json, std::string& err) const = 0;
    virtual bool get_mesh_cache_candidates(std::string& out_json, std::string& err) const = 0;
    virtual bool get_pad_state(uint32_t slot, std::string& out_json, std::string& err) const = 0;
    virtual bool set_pad_state(uint32_t slot, uint16_t buttons_mask, std::string& out_json, std::string& err) = 0;
    virtual bool tap_pad_buttons(uint32_t slot, uint16_t press_mask, uint32_t hold_steps, uint32_t release_steps, std::string& out_json, std::string& err) = 0;
    virtual bool tap_pad_named_buttons(uint32_t slot, const char* names_csv, uint32_t hold_steps, uint32_t release_steps, std::string& out_json, std::string& err) = 0;
    virtual bool hold_pad_buttons(uint32_t slot, uint16_t press_mask, std::string& out_json, std::string& err) = 0;
    virtual bool hold_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err) = 0;
    virtual bool release_pad_buttons(uint32_t slot, uint16_t release_mask, std::string& out_json, std::string& err) = 0;
    virtual bool release_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err) = 0;
    virtual bool get_scene_vector_snapshot(uint32_t max_groups, uint32_t max_roots, bool include_hud, bool include_raw_triangles, std::string& out_json, std::string& err) const = 0;
    virtual bool get_scene_delta(uint32_t max_groups, std::string& out_json, std::string& err) = 0;
    virtual bool get_object_candidates(uint32_t max_objects, std::string& out_json, std::string& err) const = 0;
    virtual bool get_scene_salience_summary(uint32_t max_targets, std::string& out_json, std::string& err) const = 0;
    virtual bool get_hierarchy_candidates(uint32_t max_nodes, std::string& out_json, std::string& err) const = 0;
    virtual bool get_focus_candidate(std::string& out_json, std::string& err) const = 0;
    virtual bool step_with_pad_observation(uint32_t slot, const char* names_csv, uint32_t hold_steps, uint32_t observe_steps, uint32_t max_groups, uint32_t max_targets, std::string& out_json, std::string& err) = 0;
    // Aggregates existing observations (GTE trace, DMA2, scene, producer, transform roots)
    // into a single render-loop fingerprint JSON that the LLM consumes for classification
    // against the 6 canonical types documented in docs/PSX_RENDER_LOOP_PLAYBOOK.md.
    // Does NOT classify server-side — the LLM applies the decision tree §4 on the fingerprint.
    virtual bool match_render_pattern(uint32_t max_candidates, std::string& out_json, std::string& err) const = 0;
    virtual bool set_psx3d_mode(const char* mode, std::string& err) = 0;
    virtual bool request_psx3d_refresh(const char* reason, const char* scope, uint32_t& id, std::string& err) = 0;
    virtual bool set_gte_trace_window(uint32_t pc_start, uint32_t pc_end, uint32_t start_frame, uint32_t end_frame, bool enabled, std::string& err) = 0;
    virtual bool list_breakpoints(std::string& out_json, std::string& err) const = 0;
    virtual bool set_breakpoint_pc(uint32_t pc, std::string& err) = 0;
    virtual bool clear_breakpoint_pc(uint32_t pc, bool& removed, std::string& err) = 0;
    virtual bool clear_all_breakpoints(uint32_t& removed_count, std::string& err) = 0;
    virtual bool run_until_breakpoint(uint32_t max_steps, uint32_t& hit_pc, uint32_t& steps_done, bool& hit, std::string& err) = 0;
};

class McpServer
{
public:
    explicit McpServer(IMcpBackend& backend, IMcpCallGuard* call_guard = nullptr);
    int run_stdio(std::FILE* in, std::FILE* out);

    // TCP transport (LSP framing: Content-Length: N\r\n\r\n<body>).
    // Blocks until `stop` becomes true OR a fatal socket error occurs.
    // Accepts one client at a time; on disconnect, loops back to accept.
    // Returns 0 on graceful stop, non-zero on socket/bind/listen failure.
    // Single-client is intentional: Claude Code is the only expected client.
    int run_tcp(uint16_t port, std::atomic<bool>& stop,
                std::atomic<int>* startup_result = nullptr);

    static std::string json_escape(const std::string& s);

private:
    IMcpBackend& backend_;
    IMcpCallGuard* call_guard_{nullptr};

    static bool read_stdio_message(std::FILE* in, std::string& out_json);
    static bool write_stdio_message(std::FILE* out, const std::string& json);

    std::string handle_request(const std::string& json);

    static std::string json_result(const std::string& id_raw, const std::string& result_json);
    static std::string json_error(const std::string& id_raw, int code, const char* message);
    static std::string extract_id_raw(const std::string& json);
    static std::string extract_json_string(const std::string& json, const char* key);
    static bool extract_json_uint32(const std::string& json, const char* key, uint32_t& out);
    static bool extract_json_bool(const std::string& json, const char* key, bool& out);

    std::string handle_initialize(const std::string& id_raw) const;
    std::string handle_tools_list(const std::string& id_raw) const;
    std::string handle_tools_call(const std::string& id_raw, const std::string& json);
};

} // namespace emu
