// R3000-Emu MCP Server (C++)
// Speaks MCP JSON-RPC on stdin/stdout, bridges to emulator TCP debug server.
// Usage: r3000_mcp [--port 9742]

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET close
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>

static socket_t emu_fd = (socket_t)-1;

static bool connect_emu(uint16_t port)
{
    emu_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(emu_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        return false;
    return true;
}

static std::string send_emu(const std::string& cmd)
{
    std::string msg = cmd + "\n";
    send(emu_fd, msg.c_str(), (int)msg.size(), 0);
    char buf[8192];
    int n = recv(emu_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "{\"error\":\"disconnected\"}";
    buf[n] = '\0';
    // Trim trailing newline
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return buf;
}

static std::string make_tool(const char* name, const char* desc, const char* params_json)
{
    std::string s = "{\"name\":\"";
    s += name;
    s += "\",\"description\":\"";
    s += desc;
    s += "\",\"inputSchema\":";
    s += params_json;
    s += "}";
    return s;
}

static void send_jsonrpc(const char* id_str, const std::string& result)
{
    std::string resp = "{\"jsonrpc\":\"2.0\",\"id\":";
    resp += id_str;
    resp += ",\"result\":";
    resp += result;
    resp += "}\n";
    fwrite(resp.c_str(), 1, resp.size(), stdout);
    fflush(stdout);
}

static void handle_request(const std::string& line)
{
    // Minimal JSON-RPC parsing
    auto find_str = [&](const char* key) -> std::string {
        std::string k = "\""; k += key; k += "\"";
        auto pos = line.find(k);
        if (pos == std::string::npos) return "";
        pos = line.find(':', pos + k.size());
        if (pos == std::string::npos) return "";
        // Check if value is string or number
        auto vstart = line.find_first_not_of(" \t", pos + 1);
        if (vstart == std::string::npos) return "";
        if (line[vstart] == '"')
        {
            auto end = line.find('"', vstart + 1);
            return (end != std::string::npos) ? line.substr(vstart + 1, end - vstart - 1) : "";
        }
        // Number or other
        auto end = line.find_first_of(",} \t\n", vstart);
        return line.substr(vstart, end - vstart);
    };

    std::string method = find_str("method");
    std::string id_raw = find_str("id");
    if (id_raw.empty()) id_raw = "null";

    if (method == "initialize")
    {
        send_jsonrpc(id_raw.c_str(),
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"r3000-emu-debug\",\"version\":\"1.0\"}}");
        return;
    }

    if (method == "notifications/initialized")
        return; // no response needed

    if (method == "tools/list")
    {
        std::string tools = "{\"tools\":[";
        tools += make_tool("read_cpu", "Read CPU registers (PC, GPR, COP0)", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_memory", "Read emulator RAM at address",
            "{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"number\",\"description\":\"RAM address (hex or decimal)\"},\"size\":{\"type\":\"number\",\"description\":\"Bytes to read (default 64)\"}}}");
        tools += ",";
        tools += make_tool("read_cdrom", "Read CDROM state (LBA, IRQ, reading)", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_timers", "Read Timer 0/1/2 state", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_gpu", "Read GPU state (GPUSTAT, frame count)", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_istat", "Read I_STAT/I_MASK interrupt state", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_game_state", "Read Soul Reaver game variables", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("read_dma", "Read all 7 DMA channel states", "{\"type\":\"object\",\"properties\":{}}");
        tools += ",";
        tools += make_tool("ping", "Check emulator connection", "{\"type\":\"object\",\"properties\":{}}");
        tools += "]}";
        send_jsonrpc(id_raw.c_str(), tools.c_str());
        return;
    }

    if (method == "tools/call")
    {
        std::string tool_name = find_str("name");
        std::string emu_cmd;

        if (tool_name == "read_memory")
        {
            std::string addr_s = find_str("addr");
            std::string size_s = find_str("size");
            uint32_t addr = addr_s.empty() ? 0 : (uint32_t)strtoul(addr_s.c_str(), nullptr, 0);
            uint32_t sz = size_s.empty() ? 64 : (uint32_t)strtoul(size_s.c_str(), nullptr, 0);
            char tmp[128]; snprintf(tmp, sizeof(tmp), "{\"cmd\":\"read_memory\",\"addr\":%u,\"size\":%u}", addr, sz);
            emu_cmd = tmp;
        }
        else
        {
            emu_cmd = "{\"cmd\":\"" + tool_name + "\"}";
        }

        std::string emu_result = send_emu(emu_cmd);
        // Wrap in MCP tool result
        std::string result = "{\"content\":[{\"type\":\"text\",\"text\":";
        // Escape the JSON string
        result += "\"";
        for (char c : emu_result)
        {
            if (c == '"') result += "\\\"";
            else if (c == '\\') result += "\\\\";
            else if (c == '\n') result += "\\n";
            else result += c;
        }
        result += "\"}]}";
        send_jsonrpc(id_raw.c_str(), result.c_str());
        return;
    }

    send_jsonrpc(id_raw.c_str(), "{\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // Set stdin/stdout to binary mode
    _setmode(_fileno(stdin), 0x8000);
    _setmode(_fileno(stdout), 0x8000);
#endif

    uint16_t port = 9742;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
    }

    if (!connect_emu(port))
    {
        fprintf(stderr, "r3000_mcp: cannot connect to emulator on port %u\n", port);
        return 1;
    }

    // Read JSON-RPC from stdin, line by line
    char line[16384];
    while (fgets(line, sizeof(line), stdin))
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        handle_request(line);
    }

    CLOSE_SOCKET(emu_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
