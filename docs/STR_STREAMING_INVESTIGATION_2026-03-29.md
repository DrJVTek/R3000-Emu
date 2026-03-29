# STR Video Streaming Investigation — 2026-03-29

## Test Case
- **Demo**: `hello_strplay` (nolibgs) — plays COPY.STR (8342 frames, 320×160, 24-bit)
- **STR file**: LBA 85-100204, XA audio interleaved every 8 sectors
- **Mode**: BIOS boot (no HLE, no fast-boot), SCPH-7502 EU

## Architecture: STR Streaming Pipeline

```
CD-ROM → INT1 → StCdInterrupt → DMA3 → Ring Buffer → StGetNext → DecDCTvlc → DMA0 → MDEC → DMA1 → VRAM
```

### Key Components
- **StCdInterrupt**: game's CD IRQ handler, receives each sector via INT1, DMA3s it to ring buffer
- **Ring buffer**: circular buffer in RAM at ~0x801F0360, 21 slots × 2016 bytes
- **StGetNext**: returns pointer to completed frame in ring buffer
- **DecDCTvlc**: Huffman BS→MDEC format converter (game-linked at 0x80015A78, NOT BIOS B(36h))
- **MDEC**: hardware decoder (RLE→IDCT→YUV→RGB)

## Root Cause Found: TIMING

### The Bug
**DecDCTvlc is called BEFORE all sectors of a frame arrive in the ring buffer.**

Timeline observed:
```
DECDCTVLC #2 CALL: a0(bs)=0x801F5A00    ← reads frame 2 from ring buffer
DMA3_RING vblank=490: wrote LBA 98 data  ← sector 2 of frame 2 arrives AFTER!
```

Frame 2 has 10 sectors (LBAs 97-107, skip 100=XA). DecDCTvlc starts reading when only sector 0 (LBA 97) is in the ring buffer. Sectors 1-9 arrive later → ring buffer has zeros → DecDCTvlc reads zeros → Huffman decoder produces garbage → MDEC gets incomplete data → only 79/200 macroblocks decoded.

### Why It Works on DuckStation
On real PS1 and DuckStation, sectors arrive one by one at ~13ms intervals (double-speed). The game's StCdInterrupt accumulates all 10 sectors, then StGetNext marks the frame as complete. DecDCTvlc only runs after ALL sectors are present.

In our emulator, the CD timing or INT1 delivery causes the game to think the frame is complete after just 1 sector.

## Fixes Applied This Session (CD-ROM)

### 1. Eight Sector Buffers (cdrom.h/cpp)
Replaced single FIFO with 8 rotating sector buffers (like DuckStation):
```cpp
static constexpr int kNumSB = 8;
struct SB { uint8_t data[2352]{}; uint16_t pos{0}; uint16_t sz{0}; };
SB sb_[kNumSB]{};
uint8_t sb_w_{0}; // write index
uint8_t sb_r_{0}; // read index
```

### 2. sb_r_ Update in All try_fill Call Sites
Fixed in 3 places: INT1 delivery (tick), Request register write, try_redeliver_sector.
```cpp
const uint8_t sb_w_before = sb_w_;
try_fill_data_fifo();
if (sb_w_ != sb_w_before)
    sb_r_ = (sb_w_ + kNumSB - 1) % kNumSB;
```

### 3. Double-Push Guard
Added `is_fifo_empty()` check in try_fill to prevent pushing the same sector twice when game writes BFRD while data already available.

### 4. XA Filter After Advance
Moved XA audio check to AFTER `read_lba_++` so we check the sector being delivered, not the previous one. Added fallback in INT1 delivery that skips INT1 if try_fill didn't push (XA caught by try_fill's internal filter).

### 5. check_sector_read_complete Simplified
Removed auto-promote logic (was causing duplicate sector reads that confused StCdInterrupt).

## Diagnostic Data

### Frame Structure on Disc
```
Frame 1: LBAs 85-96  (11 sectors, 1 XA at 92), 1812 bytes BS, version 2
Frame 2: LBAs 97-107 (10 sectors, 1 XA at 100), 14432 bytes BS, version 2
Frame 3: LBAs 109-120 (11 sectors, 1 XA at 108,116), 14388 bytes BS
```

### Sector Delivery (Verified Correct)
All 22 data sectors delivered to correct ring buffer addresses via DMA3.

### VLC Standalone Test (Python)
Standalone Huffman decoder with same tables and data produces correct output:
- 200 macroblocks, 19767 halfwords output
- End marker found at ip=14430 (2 bytes before end of 14432-byte BS data)
- **Proves: BS data on disc is valid, Huffman tables correct, algorithm correct**

### DMA0 MDEC Input Analysis
```
Frame 1: 1216 words, 1216 nonzero (100%) → 200 MBs decoded ✓
Frame 2: 9888 words, 1422 nonzero (14%) → 79 MBs decoded ✗
Frame 3: 9888 words, 1357 nonzero (14%) → 78 MBs decoded ✗
```
Frames 2-3 have ~1357 nonzero words ≈ 1 sector of VLC output. Rest is zeros from uninitialized ring buffer.

### MDEC Decode Analysis
```
Frame 1: MB[1-200] = 12 hw/MB (simple black). Complete. ✓
Frame 2: MB[1-49] = 12 hw/MB, MB[50+] = 384 hw/MB (zeros = all coefficients filled individually)
```

### Key Addresses (hello_strplay)
| Item | Address |
|------|---------|
| DecDCTvlc | 0x80015A78 |
| Primary Huffman table | 0x8001CEF0 (8192×8 bytes) |
| Secondary Huffman table | 0x8002CEF0 (512×4 bytes) |
| VLC size limit (DAT_8001cec0) | 0x00FFFFFF (unlimited) |
| Ring buffer start | ~0x801F0360 |
| Frame 2 BS data | 0x801F5A00 |
| Frame 2 MDEC buffer | 0x801BDF60 |
| DMA1 output (double-buffered) | 0x801BA360 / 0x801BC160 |

## Next Steps

1. **Fix CD timing**: ensure all sectors of a frame arrive in the ring buffer BEFORE StGetNext returns the frame as complete. The issue is likely in how INT1 delivery interacts with StCdInterrupt's frame completion logic.

2. **Investigate StCdInterrupt**: the game's callback may be seeing a "last sector" indication too early. Check if the STR sector header's `sector_num == sector_count - 1` check fires on a wrong sector.

3. **Compare INT1 delivery timing with DuckStation**: our sectors may arrive in bursts (multiple INT1s in the same tick batch) rather than spread across time. This could cause StCdInterrupt to process sector 0, see it's not the last, and return — but then the game's main loop calls StGetNext before the next INT1 arrives.
