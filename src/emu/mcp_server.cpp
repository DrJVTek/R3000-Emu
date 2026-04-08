#include "mcp_server.h"

#include "log/emu_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace emu
{

McpServer::McpServer(IMcpBackend& backend) : backend_(backend)
{
}

static std::string trim_copy(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a]))
        ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

bool McpServer::read_stdio_message(std::FILE* in, std::string& out_json)
{
    out_json.clear();
    if (!in)
        return false;

    char line[512];
    int content_length = -1;
    bool saw_header = false;
    for (;;)
    {
        if (!std::fgets(line, sizeof(line), in))
            return false;
        saw_header = true;
        const std::string raw(line);
        const std::string trimmed = trim_copy(raw);
        if (trimmed.empty())
            break;
        if (trimmed.rfind("Content-Length:", 0) == 0)
        {
            content_length = std::atoi(trimmed.c_str() + 15);
        }
    }

    if (content_length >= 0)
    {
        out_json.resize((size_t)content_length);
        const size_t got = std::fread(out_json.data(), 1, (size_t)content_length, in);
        if (got != (size_t)content_length)
        {
            emu::logf(emu::LogLevel::warn, "MCP", "stdio read short body got=%u want=%u",
                (unsigned)got, (unsigned)content_length);
            return false;
        }
        emu::logf(emu::LogLevel::debug, "MCP", "stdio read body bytes=%u", (unsigned)content_length);
        return true;
    }

    if (!saw_header)
        return false;
    return false;
}

bool McpServer::write_stdio_message(std::FILE* out, const std::string& json)
{
    if (!out)
        return false;
    std::fprintf(out, "Content-Length: %zu\r\n\r\n", json.size());
    if (!json.empty())
        std::fwrite(json.data(), 1, json.size(), out);
    std::fflush(out);
    emu::logf(emu::LogLevel::debug, "MCP", "stdio wrote body bytes=%u", (unsigned)json.size());
    return true;
}

std::string McpServer::json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)(unsigned char)c);
                out += buf;
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

std::string McpServer::json_result(const std::string& id_raw, const std::string& result_json)
{
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + (id_raw.empty() ? "null" : id_raw) + ",\"result\":" + result_json + "}";
}

std::string McpServer::json_error(const std::string& id_raw, int code, const char* message)
{
    char code_buf[32];
    std::snprintf(code_buf, sizeof(code_buf), "%d", code);
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + (id_raw.empty() ? "null" : id_raw) +
        ",\"error\":{\"code\":" + code_buf + ",\"message\":\"" + json_escape(message ? message : "error") + "\"}}";
}

std::string McpServer::extract_id_raw(const std::string& json)
{
    const size_t p = json.find("\"id\"");
    if (p == std::string::npos)
        return {};
    const size_t colon = json.find(':', p);
    if (colon == std::string::npos)
        return {};
    size_t a = colon + 1;
    while (a < json.size() && std::isspace((unsigned char)json[a]))
        ++a;
    if (a >= json.size())
        return {};
    if (json[a] == '"')
    {
        size_t b = a + 1;
        while (b < json.size())
        {
            if (json[b] == '"' && json[b - 1] != '\\')
                break;
            ++b;
        }
        if (b < json.size())
            return json.substr(a, b - a + 1);
        return {};
    }
    size_t b = a;
    while (b < json.size() && json[b] != ',' && json[b] != '}' && !std::isspace((unsigned char)json[b]))
        ++b;
    return json.substr(a, b - a);
}

std::string McpServer::extract_json_string(const std::string& json, const char* key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t p = json.find(pattern);
    if (p == std::string::npos)
        return {};
    const size_t colon = json.find(':', p + pattern.size());
    if (colon == std::string::npos)
        return {};
    size_t a = json.find('"', colon + 1);
    if (a == std::string::npos)
        return {};
    ++a;
    std::string out;
    for (size_t i = a; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '"' && json[i - 1] != '\\')
            break;
        if (c == '\\' && i + 1 < json.size())
        {
            const char n = json[i + 1];
            if (n == '"' || n == '\\' || n == '/')
            {
                out.push_back(n);
                ++i;
                continue;
            }
            if (n == 'n') { out.push_back('\n'); ++i; continue; }
            if (n == 'r') { out.push_back('\r'); ++i; continue; }
            if (n == 't') { out.push_back('\t'); ++i; continue; }
        }
        out.push_back(c);
    }
    return out;
}

bool McpServer::extract_json_uint32(const std::string& json, const char* key, uint32_t& out)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t p = json.find(pattern);
    if (p == std::string::npos)
        return false;
    const size_t colon = json.find(':', p + pattern.size());
    if (colon == std::string::npos)
        return false;
    size_t a = colon + 1;
    while (a < json.size() && std::isspace((unsigned char)json[a]))
        ++a;
    size_t b = a;
    while (b < json.size() && json[b] != ',' && json[b] != '}' && !std::isspace((unsigned char)json[b]))
        ++b;
    if (b <= a)
        return false;
    out = (uint32_t)std::strtoul(json.substr(a, b - a).c_str(), nullptr, 0);
    return true;
}

bool McpServer::extract_json_bool(const std::string& json, const char* key, bool& out)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t p = json.find(pattern);
    if (p == std::string::npos)
        return false;
    const size_t colon = json.find(':', p + pattern.size());
    if (colon == std::string::npos)
        return false;
    size_t a = colon + 1;
    while (a < json.size() && std::isspace((unsigned char)json[a]))
        ++a;
    if (json.compare(a, 4, "true") == 0)
    {
        out = true;
        return true;
    }
    if (json.compare(a, 5, "false") == 0)
    {
        out = false;
        return true;
    }
    return false;
}

std::string McpServer::handle_initialize(const std::string& id_raw) const
{
    const char* frontend = backend_.frontend_kind() == McpFrontendKind::ue5 ? "ue5" : "cli";
    const std::string result =
        std::string("{") +
        "\"protocolVersion\":\"2024-11-05\"," +
        "\"capabilities\":{\"tools\":{\"listChanged\":false}}," +
        "\"serverInfo\":{\"name\":\"r3000-emu-" + frontend + "-mcp\",\"version\":\"0.1\"}" +
        "}";
    return json_result(id_raw, result);
}

std::string McpServer::handle_tools_list(const std::string& id_raw) const
{
    const char* tools_json =
        "{"
        "\"tools\":["
        "{"
        "\"name\":\"emu.ping\","
        "\"description\":\"Checks that the emulator MCP endpoint is alive.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
        "},"
        "{"
        "\"name\":\"emu.get_status\","
        "\"description\":\"Returns high-level emulator and PSX3D status.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
        "},"
        "{"
        "\"name\":\"emu.get_cpu_state\","
        "\"description\":\"Returns the current CPU state snapshot.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
        "},"
        "{"
        "\"name\":\"emu.step\","
        "\"description\":\"Executes one or more CPU steps in the current emulation session.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100000}},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.read_ram_u32\","
        "\"description\":\"Reads a 32-bit little-endian value from physical RAM.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"phys_addr\":{\"type\":\"integer\"}},\"required\":[\"phys_addr\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.read_cop0\","
        "\"description\":\"Reads a COP0 register by index (0-31).\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"reg\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":31}},\"required\":[\"reg\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.write_cop0\","
        "\"description\":\"Writes a COP0 register by index (0-31). Useful for debug experiments.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"reg\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":31},\"value\":{\"type\":\"integer\"}},\"required\":[\"reg\",\"value\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.add_step_hook_write_cop0\","
        "\"description\":\"Adds a step hook that writes a COP0 register when a specific PC executes.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pc\":{\"type\":\"integer\"},\"reg\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":31},\"value\":{\"type\":\"integer\"},\"once\":{\"type\":\"boolean\"}},\"required\":[\"pc\",\"reg\",\"value\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.add_step_hook_write_ram_u32\","
        "\"description\":\"Adds a step hook that writes a 32-bit RAM value when a specific PC executes.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pc\":{\"type\":\"integer\"},\"phys_addr\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"},\"once\":{\"type\":\"boolean\"}},\"required\":[\"pc\",\"phys_addr\",\"value\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.list_step_hooks\","
        "\"description\":\"Lists active MCP-managed step hooks.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_step_hook\","
        "\"description\":\"Removes a step hook by id.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"hook_id\":{\"type\":\"integer\"}},\"required\":[\"hook_id\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_all_step_hooks\","
        "\"description\":\"Removes all MCP-managed step hooks.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.add_mem_watch_write\","
        "\"description\":\"Adds a low-overhead RAM write watchpoint with optional PC and value filters.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"phys_addr_start\":{\"type\":\"integer\"},\"phys_addr_end\":{\"type\":\"integer\"},\"pc\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"},\"once\":{\"type\":\"boolean\"}},\"required\":[\"phys_addr_start\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.list_mem_watches\","
        "\"description\":\"Lists active MCP-managed RAM write watchpoints.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_mem_watch\","
        "\"description\":\"Removes a RAM write watchpoint by id.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"watch_id\":{\"type\":\"integer\"}},\"required\":[\"watch_id\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_all_mem_watches\","
        "\"description\":\"Removes all RAM write watchpoints.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.list_mem_watch_events\","
        "\"description\":\"Lists recent RAM write watchpoint hits captured in the event ring buffer.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_mem_watch_events\","
        "\"description\":\"Clears the RAM write watchpoint event ring buffer.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.run_until_mem_watch\","
        "\"description\":\"Runs CPU steps until an active RAM write watchpoint hits or the step budget is exhausted.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"max_steps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100000000}},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.list_logs\","
        "\"description\":\"Lists recent emu::log entries captured by the async log sink with optional filters.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"since_seq\":{\"type\":\"integer\"},\"min_level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4},\"tag\":{\"type\":\"string\"},\"contains\":{\"type\":\"string\"},\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1024}},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_logs\","
        "\"description\":\"Clears the captured emu::log ring buffer.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.set_psx3d_mode\","
        "\"description\":\"Sets the PSX3D run mode to game or analysis.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"game\",\"analysis\"]}},\"required\":[\"mode\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.request_psx3d_refresh\"," 
        "\"description\":\"Queues a PSX3D analysis refresh request.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\"},\"scope\":{\"type\":\"string\"}},\"required\":[\"reason\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.set_gte_trace_window\","
        "\"description\":\"Configures GTE trace capture by CPU PC range and optional frame window.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pc_start\":{\"type\":\"integer\"},\"pc_end\":{\"type\":\"integer\"},\"start_frame\":{\"type\":\"integer\"},\"end_frame\":{\"type\":\"integer\"},\"enabled\":{\"type\":\"boolean\"}},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.list_breakpoints\","
        "\"description\":\"Lists active CPU PC breakpoints managed by the emulator MCP backend.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.set_breakpoint_pc\","
        "\"description\":\"Adds a CPU PC breakpoint.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pc\":{\"type\":\"integer\"}},\"required\":[\"pc\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_breakpoint_pc\","
        "\"description\":\"Removes a CPU PC breakpoint.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pc\":{\"type\":\"integer\"}},\"required\":[\"pc\"],\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.clear_all_breakpoints\","
        "\"description\":\"Removes all CPU PC breakpoints.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
        "},"
        "{"
        "\"name\":\"emu.run_until_breakpoint\","
        "\"description\":\"Runs CPU steps until a managed breakpoint is hit or the step budget is exhausted.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"max_steps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100000000}},\"additionalProperties\":false}"
        "}"
        "]"
        "}";
    return json_result(id_raw, tools_json);
}

static std::string mcp_text_result(const std::string& text, const std::string& structured_json = {})
{
    std::string out =
        std::string("{") +
        "\"content\":[{\"type\":\"text\",\"text\":\"" + McpServer::json_escape(text) + "\"}]";
    if (!structured_json.empty())
        out += ",\"structuredContent\":" + structured_json;
    out += ",\"isError\":false}";
    return out;
}

std::string McpServer::handle_tools_call(const std::string& id_raw, const std::string& json)
{
    const std::string name = extract_json_string(json, "name");
    if (name.empty())
        return json_error(id_raw, -32602, "missing tool name");

    if (name == "emu.ping")
        return json_result(id_raw, mcp_text_result("pong", "{\"ok\":true}"));

    if (name == "emu.get_status")
    {
        McpStatus s{};
        if (!backend_.get_status(s))
            return json_error(id_raw, -32001, "backend status unavailable");
        const std::string data =
            std::string("{") +
            "\"has_core\":" + (s.has_core ? std::string("true") : std::string("false")) + "," +
            "\"has_cpu\":" + (s.has_cpu ? std::string("true") : std::string("false")) + "," +
            "\"analysis_enabled\":" + (s.analysis_enabled ? std::string("true") : std::string("false")) + "," +
            "\"analysis_mode\":" + (s.analysis_mode ? std::string("true") : std::string("false")) + "," +
            "\"profile_override\":" + (s.profile_override ? std::string("true") : std::string("false")) + "," +
            "\"frame_count\":" + std::to_string(s.frame_count) + "," +
            "\"tri_3d\":" + std::to_string(s.tri_3d) + "," +
            "\"tri_2d\":" + std::to_string(s.tri_2d) + "," +
            "\"profile_path\":\"" + json_escape(s.profile_path) + "\"," +
            "\"profile_game_id\":\"" + json_escape(s.profile_game_id) + "\"" +
            "}";
        return json_result(id_raw, mcp_text_result("status", data));
    }

    if (name == "emu.get_cpu_state")
    {
        McpCpuState s{};
        if (!backend_.get_cpu_state(s))
            return json_error(id_raw, -32002, "cpu state unavailable");
        std::string data = std::string("{") +
            "\"pc\":" + std::to_string(s.pc) +
            ",\"hi\":" + std::to_string(s.hi) +
            ",\"lo\":" + std::to_string(s.lo) +
            ",\"gpr\":[";
        for (int i = 0; i < 32; ++i)
        {
            if (i) data += ",";
            data += std::to_string(s.gpr[i]);
        }
        data += "]}";
        return json_result(id_raw, mcp_text_result("cpu", data));
    }

    if (name == "emu.step")
    {
        uint32_t count = 1;
        extract_json_uint32(json, "count", count);
        if (count == 0)
            count = 1;
        if (count > 100000)
            count = 100000;
        std::string err;
        if (!backend_.step(count, err))
            return json_error(id_raw, -32003, err.empty() ? "step failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step ok", std::string("{\"count\":") + std::to_string(count) + "}"));
    }

    if (name == "emu.read_ram_u32")
    {
        uint32_t phys_addr = 0;
        if (!extract_json_uint32(json, "phys_addr", phys_addr))
            return json_error(id_raw, -32602, "missing phys_addr");
        uint32_t value = 0;
        std::string err;
        if (!backend_.read_ram_u32(phys_addr, value, err))
            return json_error(id_raw, -32004, err.empty() ? "read failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("read ok", std::string("{\"phys_addr\":") + std::to_string(phys_addr) + ",\"value\":" + std::to_string(value) + "}"));
    }

    if (name == "emu.read_cop0")
    {
        uint32_t reg = 0;
        if (!extract_json_uint32(json, "reg", reg))
            return json_error(id_raw, -32602, "missing reg");
        uint32_t value = 0;
        std::string err;
        if (!backend_.read_cop0(reg, value, err))
            return json_error(id_raw, -32013, err.empty() ? "read cop0 failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("cop0 read ok",
            std::string("{\"reg\":") + std::to_string(reg) +
            ",\"value\":" + std::to_string(value) + "}"));
    }

    if (name == "emu.write_cop0")
    {
        uint32_t reg = 0;
        uint32_t value = 0;
        if (!extract_json_uint32(json, "reg", reg))
            return json_error(id_raw, -32602, "missing reg");
        if (!extract_json_uint32(json, "value", value))
            return json_error(id_raw, -32602, "missing value");
        std::string err;
        if (!backend_.write_cop0(reg, value, err))
            return json_error(id_raw, -32014, err.empty() ? "write cop0 failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("cop0 write ok",
            std::string("{\"reg\":") + std::to_string(reg) +
            ",\"value\":" + std::to_string(value) + "}"));
    }

    if (name == "emu.add_step_hook_write_cop0")
    {
        uint32_t pc = 0;
        uint32_t reg = 0;
        uint32_t value = 0;
        bool once = true;
        if (!extract_json_uint32(json, "pc", pc))
            return json_error(id_raw, -32602, "missing pc");
        if (!extract_json_uint32(json, "reg", reg))
            return json_error(id_raw, -32602, "missing reg");
        if (!extract_json_uint32(json, "value", value))
            return json_error(id_raw, -32602, "missing value");
        extract_json_bool(json, "once", once);
        uint32_t hook_id = 0;
        std::string err;
        if (!backend_.add_step_hook_write_cop0(pc, reg, value, once, hook_id, err))
            return json_error(id_raw, -32015, err.empty() ? "add step hook write_cop0 failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step hook added",
            std::string("{\"hook_id\":") + std::to_string(hook_id) +
            ",\"pc\":" + std::to_string(pc) +
            ",\"reg\":" + std::to_string(reg) +
            ",\"value\":" + std::to_string(value) +
            ",\"once\":" + (once ? "true" : "false") + "}"));
    }

    if (name == "emu.add_step_hook_write_ram_u32")
    {
        uint32_t pc = 0;
        uint32_t phys_addr = 0;
        uint32_t value = 0;
        bool once = true;
        if (!extract_json_uint32(json, "pc", pc))
            return json_error(id_raw, -32602, "missing pc");
        if (!extract_json_uint32(json, "phys_addr", phys_addr))
            return json_error(id_raw, -32602, "missing phys_addr");
        if (!extract_json_uint32(json, "value", value))
            return json_error(id_raw, -32602, "missing value");
        extract_json_bool(json, "once", once);
        uint32_t hook_id = 0;
        std::string err;
        if (!backend_.add_step_hook_write_ram_u32(pc, phys_addr, value, once, hook_id, err))
            return json_error(id_raw, -32016, err.empty() ? "add step hook write_ram_u32 failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step hook added",
            std::string("{\"hook_id\":") + std::to_string(hook_id) +
            ",\"pc\":" + std::to_string(pc) +
            ",\"phys_addr\":" + std::to_string(phys_addr) +
            ",\"value\":" + std::to_string(value) +
            ",\"once\":" + (once ? "true" : "false") + "}"));
    }

    if (name == "emu.list_step_hooks")
    {
        std::string data;
        std::string err;
        if (!backend_.list_step_hooks(data, err))
            return json_error(id_raw, -32017, err.empty() ? "list step hooks failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step hooks", data));
    }

    if (name == "emu.clear_step_hook")
    {
        uint32_t hook_id = 0;
        if (!extract_json_uint32(json, "hook_id", hook_id))
            return json_error(id_raw, -32602, "missing hook_id");
        bool removed = false;
        std::string err;
        if (!backend_.clear_step_hook(hook_id, removed, err))
            return json_error(id_raw, -32018, err.empty() ? "clear step hook failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step hook clear",
            std::string("{\"hook_id\":") + std::to_string(hook_id) +
            ",\"removed\":" + (removed ? std::string("true") : std::string("false")) + "}"));
    }

    if (name == "emu.clear_all_step_hooks")
    {
        uint32_t removed_count = 0;
        std::string err;
        if (!backend_.clear_all_step_hooks(removed_count, err))
            return json_error(id_raw, -32019, err.empty() ? "clear all step hooks failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("step hooks cleared",
            std::string("{\"removed_count\":") + std::to_string(removed_count) + "}"));
    }

    if (name == "emu.add_mem_watch_write")
    {
        uint32_t phys_addr_start = 0;
        uint32_t phys_addr_end = 0;
        uint32_t pc = 0;
        uint32_t value = 0;
        bool once = false;
        bool has_pc = false;
        bool has_value = false;
        if (!extract_json_uint32(json, "phys_addr_start", phys_addr_start))
            return json_error(id_raw, -32602, "missing phys_addr_start");
        phys_addr_end = phys_addr_start;
        (void)extract_json_uint32(json, "phys_addr_end", phys_addr_end);
        has_pc = extract_json_uint32(json, "pc", pc);
        has_value = extract_json_uint32(json, "value", value);
        extract_json_bool(json, "once", once);
        uint32_t watch_id = 0;
        std::string err;
        if (!backend_.add_mem_watch_write(phys_addr_start, phys_addr_end, has_pc, pc, has_value, value, once, watch_id, err))
            return json_error(id_raw, -32020, err.empty() ? "add mem watch failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watch added",
            std::string("{\"watch_id\":") + std::to_string(watch_id) +
            ",\"phys_addr_start\":" + std::to_string(phys_addr_start) +
            ",\"phys_addr_end\":" + std::to_string(phys_addr_end) +
            ",\"has_pc\":" + (has_pc ? "true" : "false") +
            ",\"pc\":" + std::to_string(pc) +
            ",\"has_value\":" + (has_value ? "true" : "false") +
            ",\"value\":" + std::to_string(value) +
            ",\"once\":" + (once ? "true" : "false") + "}"));
    }

    if (name == "emu.list_mem_watches")
    {
        std::string data;
        std::string err;
        if (!backend_.list_mem_watches(data, err))
            return json_error(id_raw, -32021, err.empty() ? "list mem watches failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watches", data));
    }

    if (name == "emu.clear_mem_watch")
    {
        uint32_t watch_id = 0;
        if (!extract_json_uint32(json, "watch_id", watch_id))
            return json_error(id_raw, -32602, "missing watch_id");
        bool removed = false;
        std::string err;
        if (!backend_.clear_mem_watch(watch_id, removed, err))
            return json_error(id_raw, -32022, err.empty() ? "clear mem watch failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watch clear",
            std::string("{\"watch_id\":") + std::to_string(watch_id) +
            ",\"removed\":" + (removed ? std::string("true") : std::string("false")) + "}"));
    }

    if (name == "emu.clear_all_mem_watches")
    {
        uint32_t removed_count = 0;
        std::string err;
        if (!backend_.clear_all_mem_watches(removed_count, err))
            return json_error(id_raw, -32023, err.empty() ? "clear all mem watches failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watches cleared",
            std::string("{\"removed_count\":") + std::to_string(removed_count) + "}"));
    }

    if (name == "emu.list_mem_watch_events")
    {
        std::string data;
        std::string err;
        if (!backend_.list_mem_watch_events(data, err))
            return json_error(id_raw, -32024, err.empty() ? "list mem watch events failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watch events", data));
    }

    if (name == "emu.clear_mem_watch_events")
    {
        uint32_t cleared_count = 0;
        std::string err;
        if (!backend_.clear_mem_watch_events(cleared_count, err))
            return json_error(id_raw, -32025, err.empty() ? "clear mem watch events failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mem watch events cleared",
            std::string("{\"cleared_count\":") + std::to_string(cleared_count) + "}"));
    }

    if (name == "emu.run_until_mem_watch")
    {
        uint32_t max_steps = 1000000;
        extract_json_uint32(json, "max_steps", max_steps);
        if (max_steps == 0)
            max_steps = 1;
        if (max_steps > 100000000u)
            max_steps = 100000000u;
        uint64_t event_seq = 0;
        uint32_t watch_id = 0;
        uint32_t hit_pc = 0;
        uint32_t hit_phys_addr = 0;
        uint32_t hit_value = 0;
        uint32_t hit_size = 0;
        uint32_t steps_done = 0;
        bool hit = false;
        std::string err;
        if (!backend_.run_until_mem_watch(max_steps, event_seq, watch_id, hit_pc, hit_phys_addr, hit_value, hit_size, steps_done, hit, err))
            return json_error(id_raw, -32026, err.empty() ? "run until mem watch failed" : err.c_str());
        return json_result(id_raw, mcp_text_result(hit ? "mem watch hit" : "budget exhausted",
            std::string("{\"hit\":") + (hit ? "true" : "false") +
            ",\"event_seq\":" + std::to_string(event_seq) +
            ",\"watch_id\":" + std::to_string(watch_id) +
            ",\"hit_pc\":" + std::to_string(hit_pc) +
            ",\"hit_phys_addr\":" + std::to_string(hit_phys_addr) +
            ",\"hit_value\":" + std::to_string(hit_value) +
            ",\"hit_size\":" + std::to_string(hit_size) +
            ",\"steps_done\":" + std::to_string(steps_done) +
            ",\"max_steps\":" + std::to_string(max_steps) + "}"));
    }

    if (name == "emu.list_logs")
    {
        uint32_t since_seq_lo = 0;
        uint64_t since_seq = 0;
        uint32_t min_level = 0;
        uint32_t max_entries = 128;
        const bool has_since_seq = extract_json_uint32(json, "since_seq", since_seq_lo);
        const bool has_min_level = extract_json_uint32(json, "min_level", min_level);
        if (has_since_seq)
            since_seq = since_seq_lo;
        extract_json_uint32(json, "max_entries", max_entries);
        if (max_entries == 0)
            max_entries = 1;
        if (max_entries > 1024u)
            max_entries = 1024u;
        const std::string tag = extract_json_string(json, "tag");
        const std::string contains = extract_json_string(json, "contains");
        std::string data;
        std::string err;
        if (!backend_.list_logs(since_seq, has_min_level, min_level,
                tag.empty() ? nullptr : tag.c_str(),
                contains.empty() ? nullptr : contains.c_str(),
                max_entries, data, err))
        {
            return json_error(id_raw, -32027, err.empty() ? "list logs failed" : err.c_str());
        }
        return json_result(id_raw, mcp_text_result("logs", data));
    }

    if (name == "emu.clear_logs")
    {
        uint32_t cleared_count = 0;
        std::string err;
        if (!backend_.clear_logs(cleared_count, err))
            return json_error(id_raw, -32028, err.empty() ? "clear logs failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("logs cleared",
            std::string("{\"cleared_count\":") + std::to_string(cleared_count) + "}"));
    }

    if (name == "emu.set_psx3d_mode")
    {
        const std::string mode = extract_json_string(json, "mode");
        if (mode.empty())
            return json_error(id_raw, -32602, "missing mode");
        std::string err;
        if (!backend_.set_psx3d_mode(mode.c_str(), err))
            return json_error(id_raw, -32005, err.empty() ? "set mode failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("mode set", std::string("{\"mode\":\"") + json_escape(mode) + "\"}"));
    }

    if (name == "emu.request_psx3d_refresh")
    {
        const std::string reason = extract_json_string(json, "reason");
        std::string scope = extract_json_string(json, "scope");
        if (reason.empty())
            return json_error(id_raw, -32602, "missing reason");
        if (scope.empty())
            scope = "global";
        uint32_t refresh_id = 0;
        std::string err;
        if (!backend_.request_psx3d_refresh(reason.c_str(), scope.c_str(), refresh_id, err))
            return json_error(id_raw, -32006, err.empty() ? "refresh failed" : err.c_str());
        const std::string data =
            std::string("{\"id\":") + std::to_string(refresh_id) +
            ",\"reason\":\"" + json_escape(reason) +
            "\",\"scope\":\"" + json_escape(scope) + "\"}";
        return json_result(id_raw, mcp_text_result("refresh queued", data));
    }

    if (name == "emu.set_gte_trace_window")
    {
        uint32_t pc_start = 0;
        uint32_t pc_end = 0;
        uint32_t start_frame = 0;
        uint32_t end_frame = 0;
        bool enabled = true;
        extract_json_uint32(json, "pc_start", pc_start);
        extract_json_uint32(json, "pc_end", pc_end);
        extract_json_uint32(json, "start_frame", start_frame);
        extract_json_uint32(json, "end_frame", end_frame);
        extract_json_bool(json, "enabled", enabled);
        std::string err;
        if (!backend_.set_gte_trace_window(pc_start, pc_end, start_frame, end_frame, enabled, err))
            return json_error(id_raw, -32012, err.empty() ? "set gte trace window failed" : err.c_str());
        const std::string data =
            std::string("{\"enabled\":") + (enabled ? "true" : "false") +
            ",\"pc_start\":" + std::to_string(pc_start) +
            ",\"pc_end\":" + std::to_string(pc_end) +
            ",\"start_frame\":" + std::to_string(start_frame) +
            ",\"end_frame\":" + std::to_string(end_frame) + "}";
        return json_result(id_raw, mcp_text_result("gte trace window set", data));
    }

    if (name == "emu.list_breakpoints")
    {
        std::string data;
        std::string err;
        if (!backend_.list_breakpoints(data, err))
            return json_error(id_raw, -32007, err.empty() ? "list breakpoints failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("breakpoints", data));
    }

    if (name == "emu.set_breakpoint_pc")
    {
        uint32_t pc = 0;
        if (!extract_json_uint32(json, "pc", pc))
            return json_error(id_raw, -32602, "missing pc");
        std::string err;
        if (!backend_.set_breakpoint_pc(pc, err))
            return json_error(id_raw, -32008, err.empty() ? "set breakpoint failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("breakpoint set",
            std::string("{\"pc\":") + std::to_string(pc) + "}"));
    }

    if (name == "emu.clear_breakpoint_pc")
    {
        uint32_t pc = 0;
        if (!extract_json_uint32(json, "pc", pc))
            return json_error(id_raw, -32602, "missing pc");
        bool removed = false;
        std::string err;
        if (!backend_.clear_breakpoint_pc(pc, removed, err))
            return json_error(id_raw, -32009, err.empty() ? "clear breakpoint failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("breakpoint clear",
            std::string("{\"pc\":") + std::to_string(pc) +
            ",\"removed\":" + (removed ? std::string("true") : std::string("false")) + "}"));
    }

    if (name == "emu.clear_all_breakpoints")
    {
        uint32_t removed_count = 0;
        std::string err;
        if (!backend_.clear_all_breakpoints(removed_count, err))
            return json_error(id_raw, -32010, err.empty() ? "clear all breakpoints failed" : err.c_str());
        return json_result(id_raw, mcp_text_result("breakpoints cleared",
            std::string("{\"removed_count\":") + std::to_string(removed_count) + "}"));
    }

    if (name == "emu.run_until_breakpoint")
    {
        uint32_t max_steps = 1000000;
        extract_json_uint32(json, "max_steps", max_steps);
        if (max_steps == 0)
            max_steps = 1;
        if (max_steps > 100000000u)
            max_steps = 100000000u;
        uint32_t hit_pc = 0;
        uint32_t steps_done = 0;
        bool hit = false;
        std::string err;
        if (!backend_.run_until_breakpoint(max_steps, hit_pc, steps_done, hit, err))
            return json_error(id_raw, -32011, err.empty() ? "run until breakpoint failed" : err.c_str());
        return json_result(id_raw, mcp_text_result(hit ? "breakpoint hit" : "budget exhausted",
            std::string("{\"hit\":") + (hit ? "true" : "false") +
            ",\"hit_pc\":" + std::to_string(hit_pc) +
            ",\"steps_done\":" + std::to_string(steps_done) +
            ",\"max_steps\":" + std::to_string(max_steps) + "}"));
    }

    return json_error(id_raw, -32601, "unknown tool");
}

std::string McpServer::handle_request(const std::string& json)
{
    const std::string id_raw = extract_id_raw(json);
    const std::string method = extract_json_string(json, "method");
    if (method.empty())
        return json_error(id_raw, -32600, "missing method");

    if (method == "initialize")
        return handle_initialize(id_raw);
    if (method == "tools/list")
        return handle_tools_list(id_raw);
    if (method == "tools/call")
        return handle_tools_call(id_raw, json);
    if (method == "notifications/initialized")
        return {};

    return json_error(id_raw, -32601, "unknown method");
}

int McpServer::run_stdio(std::FILE* in, std::FILE* out)
{
    std::string req;
    while (read_stdio_message(in, req))
    {
        emu::logf(emu::LogLevel::debug, "MCP", "request method=%s",
            extract_json_string(req, "method").c_str());
        const std::string resp = handle_request(req);
        if (!resp.empty() && !write_stdio_message(out, resp))
            return 1;
    }
    emu::logf(emu::LogLevel::warn, "MCP", "stdio loop ended");
    return 0;
}

} // namespace emu
