#!/usr/bin/env python3
"""
MCP Server for DuckStation GDB Remote Debugging.

Connects to DuckStation's GDB server (default localhost:2346) via the
GDB Remote Serial Protocol (RSP) and exposes memory read, register read,
and breakpoint tools through MCP.

Usage in .claude/mcp.json:
{
  "mcpServers": {
    "duckstation": {
      "command": "python",
      "args": ["E:/Projects/github/Live/R3000-Emu/scripts/mcp_duckstation.py"]
    }
  }
}
"""
import asyncio
import socket
import struct
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent

GDB_HOST = "127.0.0.1"
GDB_PORT = 2346

# PS1 MIPS register names (GDB order)
PS1_REGS = [
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra",
    "sr","lo","hi","bad","cause","pc",
]


class GDBClient:
    """Minimal GDB Remote Serial Protocol client."""

    def __init__(self, host: str = GDB_HOST, port: int = GDB_PORT):
        self.host = host
        self.port = port
        self.sock: socket.socket | None = None

    def connect(self):
        if self.sock:
            self.close()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect((self.host, self.port))

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _checksum(self, data: str) -> str:
        return f"{sum(ord(c) for c in data) & 0xFF:02x}"

    def _send(self, cmd: str) -> str:
        if not self.sock:
            self.connect()
        packet = f"${cmd}#{self._checksum(cmd)}"
        self.sock.sendall(packet.encode("ascii"))
        return self._recv()

    def _recv(self) -> str:
        buf = b""
        while True:
            ch = self.sock.recv(1)
            if not ch:
                raise ConnectionError("GDB connection closed")
            buf += ch
            if ch == b"#":
                # read 2 checksum chars
                buf += self.sock.recv(2)
                break
        # Send ACK
        self.sock.sendall(b"+")
        # Also consume any leading '+' or ACK from DuckStation
        text = buf.decode("ascii", errors="replace")
        # Strip $...#xx wrapper
        start = text.find("$")
        end = text.rfind("#")
        if start >= 0 and end > start:
            return text[start + 1 : end]
        return text

    def read_memory(self, addr: int, length: int) -> bytes:
        """Read `length` bytes from address `addr`."""
        resp = self._send(f"m{addr:x},{length:x}")
        if resp.startswith("E"):
            raise RuntimeError(f"GDB error reading 0x{addr:08X}: {resp}")
        return bytes.fromhex(resp)

    def read_u32(self, addr: int) -> int:
        data = self.read_memory(addr, 4)
        return struct.unpack("<I", data)[0]

    def read_u16(self, addr: int) -> int:
        data = self.read_memory(addr, 2)
        return struct.unpack("<H", data)[0]

    def read_u8(self, addr: int) -> int:
        return self.read_memory(addr, 1)[0]

    def read_registers(self) -> dict[str, int]:
        """Read all CPU registers."""
        resp = self._send("g")
        if resp.startswith("E"):
            raise RuntimeError(f"GDB error reading registers: {resp}")
        regs = {}
        data = bytes.fromhex(resp)
        for i, name in enumerate(PS1_REGS):
            if i * 4 + 4 <= len(data):
                regs[name] = struct.unpack("<I", data[i * 4 : i * 4 + 4])[0]
        return regs

    def write_memory(self, addr: int, data: bytes):
        """Write bytes to address."""
        hex_data = data.hex()
        resp = self._send(f"M{addr:x},{len(data):x}:{hex_data}")
        if resp != "OK":
            raise RuntimeError(f"GDB error writing 0x{addr:08X}: {resp}")

    def set_breakpoint(self, addr: int) -> str:
        """Set a software breakpoint."""
        resp = self._send(f"Z0,{addr:x},4")
        return resp

    def remove_breakpoint(self, addr: int) -> str:
        """Remove a software breakpoint."""
        resp = self._send(f"z0,{addr:x},4")
        return resp

    def continue_exec(self) -> str:
        """Continue execution."""
        resp = self._send("c")
        return resp

    def step(self) -> str:
        """Single step."""
        resp = self._send("s")
        return resp

    def halt(self) -> str:
        """Halt execution (send break)."""
        if not self.sock:
            self.connect()
        self.sock.sendall(b"\x03")
        return self._recv()


# --- MCP Server ---

app = Server("duckstation-gdb")
gdb = GDBClient()


@app.list_tools()
async def list_tools():
    return [
        Tool(
            name="ds_connect",
            description="Connect to DuckStation GDB server (default localhost:2346)",
            inputSchema={
                "type": "object",
                "properties": {
                    "host": {"type": "string", "default": "127.0.0.1"},
                    "port": {"type": "integer", "default": 2346},
                },
            },
        ),
        Tool(
            name="ds_read_memory",
            description="Read N bytes from PS1 RAM address. Returns hex dump.",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {
                        "type": "string",
                        "description": "Hex address, e.g. '0x800C1234'",
                    },
                    "length": {
                        "type": "integer",
                        "description": "Number of bytes to read",
                        "default": 64,
                    },
                },
                "required": ["address"],
            },
        ),
        Tool(
            name="ds_read_u32",
            description="Read a 32-bit value from PS1 RAM. Returns hex + decimal.",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {"type": "string", "description": "Hex address"},
                },
                "required": ["address"],
            },
        ),
        Tool(
            name="ds_read_registers",
            description="Read all PS1 CPU registers (GPR + PC + SR + HI/LO).",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="ds_set_breakpoint",
            description="Set a software breakpoint at address.",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {"type": "string", "description": "Hex address"},
                },
                "required": ["address"],
            },
        ),
        Tool(
            name="ds_remove_breakpoint",
            description="Remove a software breakpoint at address.",
            inputSchema={
                "type": "object",
                "properties": {
                    "address": {"type": "string", "description": "Hex address"},
                },
                "required": ["address"],
            },
        ),
        Tool(
            name="ds_continue",
            description="Continue PS1 execution (returns on next break/signal).",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="ds_halt",
            description="Halt PS1 execution (send break signal).",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="ds_step",
            description="Single-step one PS1 instruction.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="ds_soul_reaver_state",
            description="Read Soul Reaver specific state: vi, st, cb6e4, d9c0, d9ac, dma1.",
            inputSchema={"type": "object", "properties": {}},
        ),
    ]


def parse_addr(s: str) -> int:
    s = s.strip()
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return int(s, 16) if all(c in "0123456789abcdefABCDEF" for c in s) else int(s)


def hex_dump(data: bytes, base_addr: int) -> str:
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off : off + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append(f"  {base_addr + off:08X}: {hex_part:<48s} {ascii_part}")
    return "\n".join(lines)


@app.call_tool()
async def call_tool(name: str, arguments: dict):
    try:
        if name == "ds_connect":
            host = arguments.get("host", GDB_HOST)
            port = arguments.get("port", GDB_PORT)
            gdb.host = host
            gdb.port = port
            gdb.connect()
            return [TextContent(type="text", text=f"Connected to DuckStation GDB at {host}:{port}")]

        if name == "ds_read_memory":
            addr = parse_addr(arguments["address"])
            length = arguments.get("length", 64)
            data = gdb.read_memory(addr, length)
            dump = hex_dump(data, addr)
            return [TextContent(type="text", text=f"Memory at 0x{addr:08X} ({length} bytes):\n{dump}")]

        if name == "ds_read_u32":
            addr = parse_addr(arguments["address"])
            val = gdb.read_u32(addr)
            return [TextContent(type="text", text=f"[0x{addr:08X}] = 0x{val:08X} ({val})")]

        if name == "ds_read_registers":
            regs = gdb.read_registers()
            lines = []
            for i, (name_r, val) in enumerate(regs.items()):
                lines.append(f"  {name_r:>5s} = 0x{val:08X}")
                if (i + 1) % 4 == 0:
                    lines.append("")
            return [TextContent(type="text", text="PS1 CPU Registers:\n" + "\n".join(lines))]

        if name == "ds_set_breakpoint":
            addr = parse_addr(arguments["address"])
            resp = gdb.set_breakpoint(addr)
            return [TextContent(type="text", text=f"Breakpoint at 0x{addr:08X}: {resp}")]

        if name == "ds_remove_breakpoint":
            addr = parse_addr(arguments["address"])
            resp = gdb.remove_breakpoint(addr)
            return [TextContent(type="text", text=f"Remove breakpoint 0x{addr:08X}: {resp}")]

        if name == "ds_continue":
            resp = gdb.continue_exec()
            return [TextContent(type="text", text=f"Continue: {resp}")]

        if name == "ds_halt":
            resp = gdb.halt()
            return [TextContent(type="text", text=f"Halt: {resp}")]

        if name == "ds_step":
            resp = gdb.step()
            regs = gdb.read_registers()
            pc = regs.get("pc", 0)
            return [TextContent(type="text", text=f"Step: PC=0x{pc:08X}")]

        if name == "ds_soul_reaver_state":
            # Soul Reaver (France) SLES-02024 specific addresses
            # These are the addresses used in our SR_STATE trace
            vi = gdb.read_u32(0x800CC9D4)      # video index
            st = gdb.read_u32(0x800CC9D0)      # state
            cb6e4 = gdb.read_u32(0x800CB6E4)   # cinemax pointer
            d9c0 = gdb.read_u32(0x800CD9C0)    # flag
            d9ac = gdb.read_u32(0x800CD9AC)     # flag
            dma1 = gdb.read_u32(0x1F801074)     # DMA ICR (I/O reg)
            regs = gdb.read_registers()
            pc = regs.get("pc", 0)
            return [TextContent(
                type="text",
                text=(
                    f"Soul Reaver State:\n"
                    f"  PC     = 0x{pc:08X}\n"
                    f"  vi     = {vi}  (video index)\n"
                    f"  st     = {st}  (state)\n"
                    f"  cb6e4  = 0x{cb6e4:08X}  (cinemax ptr)\n"
                    f"  d9c0   = {d9c0}\n"
                    f"  d9ac   = {d9ac}\n"
                    f"  dma1   = 0x{dma1:08X}\n"
                ),
            )]

        return [TextContent(type="text", text=f"Unknown tool: {name}")]

    except Exception as e:
        return [TextContent(type="text", text=f"Error: {type(e).__name__}: {e}")]


async def main():
    async with stdio_server() as (read, write):
        await app.run(read, write, app.create_initialization_options())


if __name__ == "__main__":
    asyncio.run(main())
