#!/usr/bin/env python3
"""
Parse DuckStation PSXGPU dump (.psxgpu, .psxgpu.zst) and extract GP0/GP1 commands.
Use to compare with R3000-Emu logs for debugging (offset, clip, primitives).
Format: https://github.com/ps1dev/standards/blob/main/GPUDUMP.md
"""

import struct
import sys
import os
from pathlib import Path

# Packet types
PT_GPUPort0Data = 0x00
PT_GPUPort1Data = 0x01
PT_VSyncEvent = 0x02
PT_DiscardPort0Data = 0x03
PT_ReadbackPort0Data = 0x04
PT_TraceBegin = 0x05
PT_GPUVersion = 0x06
PT_GameID = 0x10
PT_TextualVideoFormat = 0x11
PT_Comment = 0x12

# GP0 commands
GP0_ENV = {
    0xE1: "TEXPAGE",
    0xE2: "TEXWIN",
    0xE3: "CLIP_TL",
    0xE4: "CLIP_BR",
    0xE5: "DRAW_OFFSET",
    0xE6: "MASK",
}

def sign_extend_11(x):
    x = x & 0x7FF
    if x & 0x400:
        x -= 0x800
    return x

def expand5to8(x):
    return (x << 3) | (x >> 2)

def load_dump(path: str) -> bytes:
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(path)
    data = path.read_bytes()
    if path.suffix.lower() == ".zst":
        try:
            import zstandard as zstd
            dctx = zstd.ZstdDecompressor()
            data = dctx.decompress(data)
        except ImportError:
            # fallback: try zstd CLI
            import subprocess
            import tempfile
            with tempfile.NamedTemporaryFile(suffix=".psxgpu", delete=False) as f:
                tmp = f.name
            try:
                subprocess.run(["zstd", "-d", "-o", tmp, str(path)], check=True, capture_output=True)
                data = Path(tmp).read_bytes()
            finally:
                os.unlink(tmp)
    return data

def parse_header(data: bytes) -> int:
    magic = b"PSXGPUDUMP"
    if len(data) < 14 or data[:10] != magic:
        raise ValueError("Invalid PSXGPU dump header")
    return 14  # PSXGPUDUMPv1\0\0

def get_next_packet(data: bytes, pos: int):
    if pos + 4 > len(data):
        return None, pos
    hdr = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    length = hdr & 0xFFFFFF
    ptype = (hdr >> 24) & 0xFF
    payload_size = length * 4
    if pos + payload_size > len(data):
        return None, pos
    payload = data[pos:pos + payload_size]
    pos += payload_size
    return (ptype, payload), pos

def parse_gp0_env(val: int) -> str:
    cmd = (val >> 24) & 0xFF
    name = GP0_ENV.get(cmd, f"ENV_0x{cmd:02X}")
    if cmd == 0xE3:  # CLIP_TL
        x, y = val & 0x3FF, (val >> 10) & 0x1FF
        return f"  GP0 {name} ({x},{y})"
    if cmd == 0xE4:  # CLIP_BR
        x, y = val & 0x3FF, (val >> 10) & 0x1FF
        return f"  GP0 {name} ({x},{y})"
    if cmd == 0xE5:  # DRAW_OFFSET
        ox = sign_extend_11(val & 0x7FF)
        oy = sign_extend_11((val >> 11) & 0x7FF)
        return f"  GP0 {name} ({ox},{oy})"
    return f"  GP0 {name} 0x{val:08X}"

def gp0_param_count(cmd: int) -> int:
    if cmd <= 0x1F:
        return 0
    if 0x20 <= cmd <= 0x3F:  # polygons
        gouraud = (cmd & 0x10) != 0
        quad = (cmd & 0x08) != 0
        textured = (cmd & 0x04) != 0
        verts = 4 if quad else 3
        if not gouraud and not textured:
            return verts
        if not gouraud and textured:
            return verts * 2
        if gouraud and not textured:
            return verts * 2 - 1
        return verts * 3 - 1
    if 0x40 <= cmd <= 0x5F:
        if (cmd & 0x08):  # polyline
            return -1
        return 3 if (cmd & 0x10) else 2
    if 0x60 <= cmd <= 0x7F:
        size_code = (cmd >> 3) & 3
        params = 1
        if size_code == 0:
            params += 1
        if (cmd & 0x04):
            params += 1
        return params
    if 0xE0 <= cmd <= 0xFF:
        return 0  # single-word env
    return 0

def parse_gp0_stream(words: list) -> list:
    """Parse GP0 word stream, return list of (consumed_count, line_str)."""
    lines = []
    i = 0
    while i < len(words):
        val = words[i]
        cmd = (val >> 24) & 0xFF
        if cmd in GP0_ENV:
            lines.append((1, parse_gp0_env(val)))
            i += 1
            continue
        if 0x20 <= cmd <= 0x3F:  # polygon
            nparams = gp0_param_count(cmd)
            if i + 1 + nparams > len(words):
                lines.append((1, f"  GP0 POLY 0x{cmd:02X} (incomplete)"))
                i += 1
                continue
            chunk = words[i:i + 1 + nparams]
            quad = (cmd & 0x08) != 0
            gouraud = (cmd & 0x10) != 0
            textured = (cmd & 0x04) != 0
            nverts = 4 if quad else 3
            flat = "FLAT" if not gouraud else "GOURAUD"
            typ = "QUAD" if quad else "TRI"
            name = f"{typ}_{flat}"
            if textured:
                name += "_TEX"
            r = expand5to8((chunk[0] >> 0) & 0x1F)
            g = expand5to8((chunk[0] >> 5) & 0x1F)
            b = expand5to8((chunk[0] >> 10) & 0x1F)
            idx = 1
            verts = []
            for j in range(nverts):
                if j > 0 and gouraud:
                    r = expand5to8((chunk[idx] >> 0) & 0x1F)
                    g = expand5to8((chunk[idx] >> 5) & 0x1F)
                    b = expand5to8((chunk[idx] >> 10) & 0x1F)
                    idx += 1
                if idx >= len(chunk):
                    break
                v = chunk[idx]
                x = sign_extend_11(v & 0x7FF)
                y = sign_extend_11((v >> 16) & 0x7FF)
                verts.append((x, y, r, g, b))
                idx += 1
                if textured:
                    idx += 1
            parts = " ".join(f"v{k}=({x},{y})#{r:02X}{g:02X}{b:02X}" for k, (x, y, r, g, b) in enumerate(verts))
            lines.append((len(chunk), f"  GP0 {name} {parts}"))
            i += len(chunk)
            continue
        if 0x40 <= cmd <= 0x5F:
            nparams = gp0_param_count(cmd)
            if nparams < 0:  # polyline - scan for terminator
                j = i + 1
                while j < len(words):
                    w = words[j]
                    if (w & 0xF000F000) == 0x50005000:
                        break
                    j += 2 if (cmd & 0x10) else 1
                nparams = j - i - 1
            if i + 1 + nparams <= len(words):
                lines.append((1 + nparams, f"  GP0 LINE (skip {nparams+1} words)"))
                i += 1 + nparams
            else:
                lines.append((1, f"  GP0 LINE 0x{cmd:02X} (incomplete)"))
                i += 1
            continue
        if 0x60 <= cmd <= 0x7F:
            nparams = gp0_param_count(cmd)
            if i + 1 + nparams <= len(words):
                lines.append((1 + nparams, f"  GP0 RECT 0x{cmd:02X} (skip)"))
                i += 1 + nparams
            else:
                lines.append((1, f"  GP0 RECT 0x{cmd:02X} (incomplete)"))
                i += 1
            continue
        if cmd == 0xA0:  # CPU to VRAM
            if i + 3 <= len(words):
                n = (words[i + 1] & 0xFFFF) * (words[i + 2] & 0xFFFF)
                nwords = 3 + (n + 1) // 2
                lines.append((min(nwords, len(words) - i), f"  GP0 VRAM_WRITE ~{n} pixels"))
                i += min(nwords, len(words) - i)
            else:
                i += 1
            continue
        lines.append((1, f"  GP0 0x{val:08X} (cmd=0x{cmd:02X})"))
        i += 1
    return lines

def main():
    if len(sys.argv) < 2:
        print("Usage: parse_psxgpu_dump.py <file.psxgpu|file.psxgpu.zst> [frame_index|--list|--pre-trace|--vram]")
        print("  --vram: extract VRAM and output as ASCII art + optional .ppm")
        sys.exit(1)
    path = sys.argv[1]
    frame_idx = None
    if len(sys.argv) > 2:
        if sys.argv[2] == "--list":
            frame_idx = "list"
        elif sys.argv[2] == "--pre-trace":
            frame_idx = "pretrace"
        elif sys.argv[2] == "--vram":
            frame_idx = "vram"
        else:
            frame_idx = int(sys.argv[2])

    data = load_dump(path)
    pos = parse_header(data)
    in_trace = False
    frame_start_offsets = []
    current_frame = -1

    # --list: show packet structure; --pre-trace: dump GP0/GP1 before TraceBegin
    if frame_idx == "list" or frame_idx == "pretrace":
        pos = parse_header(data)
        n = 0
        dump_content = frame_idx == "pretrace"
        while pos < len(data) and n < 200:
            pkt, pos = get_next_packet(data, pos)
            if pkt is None:
                break
            ptype, payload = pkt
            names = {0:"GP0", 1:"GP1", 2:"VSync", 3:"Discard", 4:"Readback", 5:"TraceBegin",
                     6:"GPUVer", 0x10:"GameID", 0x11:"VideoFmt", 0x12:"Comment"}
            print(f"  {n:3d} type=0x{ptype:02X} {names.get(ptype,'?')} len={len(payload)//4}")
            if dump_content and ptype == 0 and len(payload) >= 4:
                words = list(struct.unpack_from(f"<{len(payload)//4}I", payload, 0))
                if len(words) <= 20:
                    for _, line in parse_gp0_stream(words):
                        print(f"       {line.strip()}")
                elif len(words) >= 3 and (words[0] >> 24) == 0xA0 and words[1] == 0 and words[2] == 0:
                    print(f"       [VRAM init {len(words)-3} words]")
            if dump_content and ptype == 1 and len(payload) >= 4:
                w = struct.unpack_from("<I", payload, 0)[0]
                print(f"       GP1 0x{(w>>24)&0x3F:02X} = 0x{w:08X}")
            if ptype == 5:
                print("  ^ TraceBegin")
                break
            n += 1
        return

    # --vram: extract VRAM and output as ASCII art
    if frame_idx == "vram":
        pos = parse_header(data)
        vram_words = None
        while pos < len(data):
            pkt, pos = get_next_packet(data, pos)
            if pkt is None:
                break
            ptype, payload = pkt
            if ptype == 0 and len(payload) >= 4:
                words = list(struct.unpack_from(f"<{len(payload)//4}I", payload, 0))
                if len(words) >= 262147 and (words[0] >> 24) == 0xA0 and words[1] == 0 and words[2] == 0:
                    vram_words = words[3:3 + 262144]
                    break
        if not vram_words:
            print("VRAM not found in dump")
            sys.exit(1)

        # PS1 VRAM: 1024x512, 16-bit pixels. Format: XBBBBBGG GGGRRRRR (or R|G<<5|B<<10)
        # Bits 0-4=R, 5-9=G, 10-14=B (15-bit color)
        W, H = 1024, 512

        def px_to_bright(px16):
            r = (px16 >> 0) & 0x1F
            g = (px16 >> 5) & 0x1F
            b = (px16 >> 10) & 0x1F
            return (r * 255 // 31 + g * 255 // 31 + b * 255 // 31) // 3

        # Build 2D array (row-major: y*1024+x, each word = 2 pixels)
        pixels = []
        for i in range(0, len(vram_words) * 2, 2):
            w = vram_words[i // 2]
            pixels.append(px_to_bright(w & 0xFFFF))
            pixels.append(px_to_bright((w >> 16) & 0xFFFF))

        # ASCII art: chars by brightness (0-255 -> 10 levels)
        chars = " .:-=+*#@"
        block = 4  # 1 char per 4x4 block -> 256x128 chars
        out_w, out_h = W // block, H // block
        lines = []
        lines.append(f"VRAM DuckStation {W}x{H} (block={block}, display area often 0-640 x 0-480)")
        lines.append("")
        for by in range(min(out_h, 120)):  # cap height for terminal
            row = []
            for bx in range(min(out_w, 160)):  # cap width
                acc = 0
                n = 0
                for dy in range(block):
                    for dx in range(block):
                        x, y = bx * block + dx, by * block + dy
                        if y < H and x < W:
                            idx = y * W + x
                            if idx < len(pixels):
                                acc += pixels[idx]
                                n += 1
                avg = acc // n if n else 0
                row.append(chars[min(9, avg * 10 // 256)])
            lines.append("".join(row))
        print("\n".join(lines))

        # Save PPM for comparison with our emulator output
        p = Path(path)
        ppm_name = str(p.parent / (p.stem.replace(".psxgpu", "") + "_vram.ppm"))
        with open(ppm_name, "wb") as f:
            f.write(f"P6\n{W} {H}\n255\n".encode())
            for y in range(H):
                for x in range(W):
                    idx = y * W + x
                    wi, sub = idx // 2, idx & 1
                    w = vram_words[wi]
                    px = (w >> (sub * 16)) & 0xFFFF
                    r = ((px >> 0) & 0x1F) * 255 // 31
                    g = ((px >> 5) & 0x1F) * 255 // 31
                    b = ((px >> 10) & 0x1F) * 255 // 31
                    f.write(bytes([r, g, b]))
        print(f"\nPPM saved: {ppm_name}")

        # Summary file for comparison with R3000-Emu mesh output
        txt_name = ppm_name.replace(".ppm", "_layout.txt")
        with open(txt_name, "w", encoding="utf-8") as f:
            f.write("DuckStation VRAM reference - Ridge Racer logo\n")
            f.write("=" * 60 + "\n")
            f.write("Display: 1024x512 VRAM, typical view 640x480 or 320x240\n")
            f.write("Pre-trace state: CLIP (0,2)-(639,479), OFFSET (0,2)\n")
            f.write("\nLogo position (from ASCII): ~center X, rows ~20-60\n")
            f.write("Our emulator: mesh rendering (no VRAM). Compare:\n")
            f.write("  - Same vertices (TRI_FLAT from logs) + offset fix\n")
            f.write("  - bCenterDisplay + clip center when offset=(0,0)\n")
            f.write("  - Origin at display center (320, 242) for 640x480\n")
            f.write("\nIf logo is shifted/wrong: check R3000GpuComponent.cpp\n")
            f.write("  vx += ClipCx when offset=0 (center-relative coords)\n")
        print(f"Layout summary: {txt_name}")
        return

    # First pass: find frame starts
    while pos < len(data):
        pkt, pos = get_next_packet(data, pos)
        if pkt is None:
            break
        ptype, payload = pkt
        if ptype == PT_TraceBegin:
            frame_start_offsets.append(pos)
            in_trace = True
        elif ptype == PT_VSyncEvent and in_trace:
            frame_start_offsets.append(pos)

    if not frame_start_offsets:
        print("No frames found")
        sys.exit(1)

    # Reset and optionally seek to frame
    pos = parse_header(data)
    in_trace = False
    current_frame = -1
    frames_to_skip = frame_idx if isinstance(frame_idx, int) else 0
    dumping = not isinstance(frame_idx, int) or frame_idx == 0

    pkt_count = 0
    while pos < len(data):
        pkt, pos = get_next_packet(data, pos)
        if pkt is None:
            break
        ptype, payload = pkt

        if ptype == PT_TraceBegin:
            current_frame += 1
            if current_frame == frames_to_skip:
                dumping = True
                print(f"\n--- Frame {current_frame} ---")
            in_trace = True
            continue
        if ptype == PT_VSyncEvent:
            if dumping and current_frame == frames_to_skip:
                print(f"  VSync")
            current_frame += 1
            if current_frame > frames_to_skip and frame_idx is not None:
                break
            if current_frame == frames_to_skip:
                dumping = True
            elif current_frame > frames_to_skip and frame_idx is not None:
                break
            continue
        if not dumping or current_frame != frames_to_skip:
            continue

        pkt_count += 1
        if ptype == PT_GPUPort0Data:
            words = list(struct.unpack_from(f"<{len(payload)//4}I", payload, 0))
            # DuckStation initial VRAM: A0, 0, 0, then raw VRAM - skip that block
            if len(words) >= 3 and (words[0] >> 24) == 0xA0 and words[1] == 0 and words[2] == 0:
                print(f"  [VRAM init {len(words)-3} words - skipped]")
            else:
                for _, line in parse_gp0_stream(words):
                    print(line)
        elif ptype == PT_GPUPort1Data and len(payload) >= 4:
            w = struct.unpack_from("<I", payload, 0)[0]
            g1 = (w >> 24) & 0x3F
            print(f"  GP1 0x{g1:02X} 0x{w:08X}")

    print(f"\nTotal frames: {len(frame_start_offsets)}, packets in frame: {pkt_count}")

if __name__ == "__main__":
    main()
