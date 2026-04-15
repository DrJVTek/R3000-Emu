#!/usr/bin/env python3
"""
Dump useful metadata from PlayStation STR / XA sectors.

Supports:
- 2336-byte raw STR sectors (XA subheader + payload)
- 2352-byte raw CD sectors

The script is intentionally conservative:
- it decodes XA subheader fields and STR video header fields we can identify safely
- it avoids guessing undocumented header words beyond the common frame/chunk/size/dimensions fields
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import math
from pathlib import Path
import re
from typing import Iterable


KNOWN_SUBMODE_BITS = (
    (0x80, "EOF"),
    (0x40, "REALTIME"),
    (0x20, "FORM2"),
    (0x10, "TRIGGER"),
    (0x08, "VIDEO"),
    (0x04, "AUDIO"),
    (0x02, "DATA"),
    (0x01, "EOR"),
)


def le16(buf: bytes, offset: int) -> int:
    return buf[offset] | (buf[offset + 1] << 8)


def le32(buf: bytes, offset: int) -> int:
    return (
        buf[offset]
        | (buf[offset + 1] << 8)
        | (buf[offset + 2] << 16)
        | (buf[offset + 3] << 24)
    )


def bcd_to_int(value: int) -> int:
    return ((value >> 4) * 10) + (value & 0x0F)


def fnv1a_hash_bytes(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def detect_sector_size(path: Path, requested: int | None) -> int:
    if requested:
        return requested

    size = path.stat().st_size
    for candidate in (2336, 2352):
        if size % candidate == 0:
            return candidate
    raise SystemExit(
        f"Unable to detect sector size for {path} (size={size}). "
        "Use --sector-size 2336 or 2352."
    )


def parse_cue_single_track(cue_path: Path) -> tuple[Path, int]:
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    file_match = re.search(r'FILE\s+"([^"]+)"\s+BINARY', text, re.IGNORECASE)
    if not file_match:
        raise SystemExit(f"Unsupported CUE layout in {cue_path}: missing FILE")
    bin_path = (cue_path.parent / file_match.group(1)).resolve()
    index_match = re.search(r"INDEX\s+01\s+(\d+):(\d+):(\d+)", text, re.IGNORECASE)
    if not index_match:
        raise SystemExit(f"Unsupported CUE layout in {cue_path}: missing INDEX 01")
    mm = int(index_match.group(1))
    ss = int(index_match.group(2))
    ff = int(index_match.group(3))
    track_lba = ((mm * 60) + ss) * 75 + ff
    return bin_path, track_lba


def read_mode2_2352_user_sector(bin_path: Path, track_lba: int, lba: int) -> bytes:
    with bin_path.open("rb") as f:
        f.seek((track_lba + lba) * 2352 + 24)
        data = f.read(2048)
    if len(data) != 2048:
        raise SystemExit(f"Failed to read sector {lba} from {bin_path}")
    return data


def parse_iso_dir_entries(dir_data: bytes) -> list[dict[str, int | str | bool]]:
    entries: list[dict[str, int | str | bool]] = []
    offset = 0
    while offset < len(dir_data):
        rec_len = dir_data[offset]
        if rec_len == 0:
            break
        entry = dir_data[offset : offset + rec_len]
        if len(entry) < 34:
            break
        lba = le32(entry, 2)
        size = le32(entry, 10)
        flags = entry[25]
        name_len = entry[32]
        raw_name = entry[33 : 33 + name_len]
        name = raw_name.decode("ascii", errors="replace")
        if ";" in name:
            name = name.split(";")[0]
        entries.append(
            {
                "name": name,
                "lba": lba,
                "size": size,
                "is_dir": (flags & 0x02) != 0,
            }
        )
        offset += rec_len
    return entries


def normalize_iso_name(name: str) -> str:
    return name.replace("\\", "/").strip().strip("/").upper()


def resolve_iso_file_from_cue(cue_path: Path, iso_path: str) -> tuple[Path, int, int]:
    bin_path, track_lba = parse_cue_single_track(cue_path)
    pvd = read_mode2_2352_user_sector(bin_path, track_lba, 16)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise SystemExit(f"{cue_path} does not look like an ISO9660 disc")

    root_lba = le32(pvd, 158)
    root_size = le32(pvd, 166)
    current_lba = root_lba
    current_size = root_size
    parts = [part for part in normalize_iso_name(iso_path).split("/") if part]
    if not parts:
        raise SystemExit("--iso-file must point to a file inside the disc, e.g. /COPY.STR")

    for index, part in enumerate(parts):
        sector_count = max(1, math.ceil(current_size / 2048))
        dir_data = bytearray()
        for rel in range(sector_count):
            dir_data.extend(read_mode2_2352_user_sector(bin_path, track_lba, current_lba + rel))
        entries = parse_iso_dir_entries(bytes(dir_data[:current_size]))
        match = None
        for entry in entries:
            entry_name = normalize_iso_name(str(entry["name"]))
            if entry_name == part:
                match = entry
                break
        if match is None:
            raise SystemExit(f"{iso_path} not found in {cue_path}")
        if index == len(parts) - 1:
            if bool(match["is_dir"]):
                raise SystemExit(f"{iso_path} resolves to a directory, not a file")
            return bin_path, int(match["lba"]), int(match["size"])
        if not bool(match["is_dir"]):
            raise SystemExit(f"{iso_path} has a non-directory component: {part}")
        current_lba = int(match["lba"])
        current_size = int(match["size"])

    raise SystemExit(f"{iso_path} not found in {cue_path}")


def submode_flags(submode: int) -> str:
    flags = [name for bit, name in KNOWN_SUBMODE_BITS if submode & bit]
    return "|".join(flags) if flags else "-"


def coding_desc(coding: int) -> str:
    channels = "stereo" if (coding & 0x01) else "mono"
    rate = "18900" if (coding & 0x04) else "37800"
    sample_bits = "8bit" if (coding & 0x10) else "4bit"
    emphasis = "emph" if (coding & 0x40) else "plain"
    return f"{channels},{rate},{sample_bits},{emphasis}"


@dataclass
class VideoHeader:
    chunk_index: int
    chunk_count: int
    frame_number: int
    used_demux_size: int
    width: int
    height: int
    aux0: int
    aux1: int
    aux2: int
    aux3: int


@dataclass
class SectorRecord:
    sector_index: int
    sector_size: int
    file_number: int
    channel_number: int
    submode: int
    coding: int
    flags: str
    payload_size: int
    mode: int | None
    minute: int | None
    second: int | None
    frame: int | None
    video: VideoHeader | None

    @property
    def kind(self) -> str:
        if self.video is not None:
            return "STR_VIDEO"
        if (self.submode & 0x04) and (self.submode & 0x40):
            return "XA_AUDIO"
        if self.submode & 0x08:
            return "VIDEO?"
        if self.submode & 0x04:
            return "AUDIO?"
        if self.submode & 0x02:
            return "DATA"
        return "OTHER"


@dataclass
class TraceRecord:
    source: str
    lba: int
    word_count: int
    hash_value: int
    w0: int
    w1: int
    w2: int
    w3: int
    line: str


@dataclass
class ExpectedRecord:
    sector_index: int
    source: str
    word_count: int
    hash_value: int
    w0: int
    w1: int
    w2: int
    w3: int


@dataclass(frozen=True)
class VideoHeaderKey:
    chunk_field: int
    frame_number: int
    demux_size: int


def parse_video_header(payload: bytes) -> VideoHeader | None:
    if len(payload) < 32:
        return None
    magic = le32(payload, 0)
    if magic != 0x80010160:
        return None
    return VideoHeader(
        chunk_index=le16(payload, 4),
        chunk_count=le16(payload, 6),
        frame_number=le32(payload, 8),
        used_demux_size=le32(payload, 12),
        width=le16(payload, 16),
        height=le16(payload, 18),
        aux0=le16(payload, 20),
        aux1=le16(payload, 22),
        aux2=le16(payload, 24),
        aux3=le16(payload, 26),
    )


def parse_sector(raw: bytes, sector_index: int, sector_size: int) -> SectorRecord:
    if sector_size == 2336:
        file_number = raw[0]
        channel_number = raw[1]
        submode = raw[2]
        coding = raw[3]
        payload = raw[8:]
        mode = None
        minute = None
        second = None
        frame = None
    elif sector_size == 2352:
        header = raw[12:16]
        subheader = raw[16:20]
        mode = header[3]
        minute = bcd_to_int(header[0])
        second = bcd_to_int(header[1])
        frame = bcd_to_int(header[2])
        file_number = subheader[0]
        channel_number = subheader[1]
        submode = subheader[2]
        coding = subheader[3]
        payload = raw[24:]
    else:
        raise ValueError(f"Unsupported sector size: {sector_size}")

    return SectorRecord(
        sector_index=sector_index,
        sector_size=sector_size,
        file_number=file_number,
        channel_number=channel_number,
        submode=submode,
        coding=coding,
        flags=submode_flags(submode),
        payload_size=len(payload),
        mode=mode,
        minute=minute,
        second=second,
        frame=frame,
        video=parse_video_header(payload),
    )


def sector_payload_slices(raw: bytes, sector_size: int) -> dict[str, bytes]:
    if sector_size == 2336:
        raw_sector = raw
        user_payload = raw[8:]
    elif sector_size == 2352:
        raw_sector = raw
        user_payload = raw[24:]
    else:
        raise ValueError(f"Unsupported sector size: {sector_size}")

    dma3_header = user_payload[: 8 * 4]
    dma3_payload = user_payload[8 * 4 : 512 * 4]
    return {
        "ISORAW": raw_sector,
        "CDUSER": user_payload[:2048],
        "CDFIFO": user_payload[:2048],
        "DMA3HDR": dma3_header,
        "DMA3PAYLOAD": dma3_payload,
    }


def iter_sector_records(path: Path, sector_size: int, start: int, count: int | None) -> Iterable[SectorRecord]:
    file_size = path.stat().st_size
    total_sectors = file_size // sector_size
    if start < 0 or start >= total_sectors:
        raise SystemExit(f"--start {start} is outside 0..{total_sectors - 1}")

    end = total_sectors if count is None else min(total_sectors, start + count)
    with path.open("rb") as f:
        f.seek(start * sector_size)
        for sector_index in range(start, end):
            raw = f.read(sector_size)
            if len(raw) != sector_size:
                break
            yield parse_sector(raw, sector_index, sector_size)


def iter_cue_file_records(cue_path: Path, iso_path: str, start: int, count: int | None) -> Iterable[tuple[int, bytes]]:
    bin_path, file_lba, file_size = resolve_iso_file_from_cue(cue_path, iso_path)
    total_sectors = math.ceil(file_size / 2048)
    end = total_sectors if count is None else min(total_sectors, start + count)
    with bin_path.open("rb") as f:
        for sector_index in range(start, end):
            abs_lba = file_lba + sector_index
            f.seek(abs_lba * 2352)
            raw = f.read(2352)
            if len(raw) != 2352:
                break
            yield sector_index, raw


def iter_sector_bytes(path: Path, sector_size: int, start: int = 0, count: int | None = None) -> Iterable[tuple[int, bytes]]:
    file_size = path.stat().st_size
    total_sectors = file_size // sector_size
    end = total_sectors if count is None else min(total_sectors, start + count)
    with path.open("rb") as f:
        f.seek(start * sector_size)
        for sector_index in range(start, end):
            raw = f.read(sector_size)
            if len(raw) != sector_size:
                break
            yield sector_index, raw


def print_sector_table(records: list[SectorRecord]) -> None:
    print(
        "sector kind      file ch sub  flags                         coding                  payload details"
    )
    print(
        "------ --------- ---- -- ---- ----------------------------- ----------------------- ------- -----------------------------"
    )
    for record in records:
        details = ""
        if record.video is not None:
            vh = record.video
            details = (
                f"frame={vh.frame_number} chunk={vh.chunk_index}/{vh.chunk_count} "
                f"demux={vh.used_demux_size} {vh.width}x{vh.height} "
                f"aux={vh.aux0:04X},{vh.aux1:04X},{vh.aux2:04X},{vh.aux3:04X}"
            )
        elif record.kind.startswith("XA_AUDIO"):
            details = "xa-audio"
        elif record.mode is not None:
            details = f"msf={record.minute:02d}:{record.second:02d}:{record.frame:02d} mode={record.mode}"

        print(
            f"{record.sector_index:6d} "
            f"{record.kind:9s} "
            f"{record.file_number:4d} "
            f"{record.channel_number:2d} "
            f"0x{record.submode:02X} "
            f"{record.flags:29.29s} "
            f"{coding_desc(record.coding):23.23s} "
            f"{record.payload_size:7d} "
            f"{details}"
        )


def print_summary(path: Path, sector_size: int, records: list[SectorRecord],
                   lba_base: int = 0) -> None:
    total = len(records)
    video_records = [r for r in records if r.video is not None]
    audio_records = [r for r in records if r.kind == "XA_AUDIO"]

    print()
    print("Summary")
    print(f"- file: {path}")
    print(f"- sector size: {sector_size}")
    print(f"- sectors dumped: {total}")
    print(f"- video sectors: {len(video_records)}")
    print(f"- xa audio sectors: {len(audio_records)}")
    if lba_base:
        print(f"- lba base: {lba_base}")

    frame_map: dict[int, list[SectorRecord]] = {}
    for record in video_records:
        frame_map.setdefault(record.video.frame_number, []).append(record)

    if frame_map:
        print("- frames:")
        for frame_number in sorted(frame_map):
            sectors = frame_map[frame_number]
            first = sectors[0].video
            chunk_count = first.chunk_count
            chunks_present = sorted({s.video.chunk_index for s in sectors})
            missing = [idx for idx in range(chunk_count) if idx not in chunks_present]
            missing_str = ",".join(str(idx) for idx in missing[:12]) if missing else "-"
            if len(missing) > 12:
                missing_str += ",..."
            first_lba = lba_base + sectors[0].sector_index
            last_lba = lba_base + sectors[-1].sector_index
            print(
                f"  frame {frame_number}: sectors={len(sectors)}/{chunk_count} "
                f"chunks={chunks_present[0]}..{chunks_present[-1]} "
                f"lba={first_lba}..{last_lba} "
                f"missing={missing_str} demux={first.used_demux_size} "
                f"size={first.width}x{first.height}"
            )


def write_csv(path: Path, records: list[SectorRecord]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "sector_index",
                "kind",
                "file_number",
                "channel_number",
                "submode_hex",
                "submode_flags",
                "coding_hex",
                "coding_desc",
                "payload_size",
                "msf_minute",
                "msf_second",
                "msf_frame",
                "mode",
                "video_frame_number",
                "video_chunk_index",
                "video_chunk_count",
                "video_used_demux_size",
                "video_width",
                "video_height",
                "video_aux0",
                "video_aux1",
                "video_aux2",
                "video_aux3",
            ]
        )
        for record in records:
            vh = record.video
            writer.writerow(
                [
                    record.sector_index,
                    record.kind,
                    record.file_number,
                    record.channel_number,
                    f"0x{record.submode:02X}",
                    record.flags,
                    f"0x{record.coding:02X}",
                    coding_desc(record.coding),
                    record.payload_size,
                    record.minute,
                    record.second,
                    record.frame,
                    record.mode,
                    vh.frame_number if vh else None,
                    vh.chunk_index if vh else None,
                    vh.chunk_count if vh else None,
                    vh.used_demux_size if vh else None,
                    vh.width if vh else None,
                    vh.height if vh else None,
                    f"0x{vh.aux0:04X}" if vh else None,
                    f"0x{vh.aux1:04X}" if vh else None,
                    f"0x{vh.aux2:04X}" if vh else None,
                    f"0x{vh.aux3:04X}" if vh else None,
                ]
            )


TRACE_PATTERNS = (
    re.compile(
        r"^\[\d+\]\s+(ISORAW|CDUSER|CDFIFO)\s+lba=(\d+)\s+bytes=(\d+)\s+hash=0x([0-9A-Fa-f]+)\s+"
        r"w0=0x([0-9A-Fa-f]+)\s+w1=0x([0-9A-Fa-f]+)\s+w2=0x([0-9A-Fa-f]+)\s+w3=0x([0-9A-Fa-f]+)"
    ),
    re.compile(
        r"^\[\d+\]\s+DMA3DATA\s+pc=0x[0-9A-Fa-f]+\s+madr=0x[0-9A-Fa-f]+\s+words=(\d+)\s+"
        r"read_lba=(\d+)\s+data_lba=(\d+)\s+hash=0x([0-9A-Fa-f]+)\s+"
        r"w0=0x([0-9A-Fa-f]+)\s+w1=0x([0-9A-Fa-f]+)\s+w2=0x([0-9A-Fa-f]+)\s+w3=0x([0-9A-Fa-f]+)"
    ),
)


def parse_trace_records(trace_path: Path) -> list[TraceRecord]:
    records: list[TraceRecord] = []
    with trace_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            match = TRACE_PATTERNS[0].match(line)
            if match:
                source, lba_s, bytes_s, hash_s, w0_s, w1_s, w2_s, w3_s = match.groups()
                records.append(
                    TraceRecord(
                        source=source,
                        lba=int(lba_s),
                        word_count=int(bytes_s) // 4,
                        hash_value=int(hash_s, 16),
                        w0=int(w0_s, 16),
                        w1=int(w1_s, 16),
                        w2=int(w2_s, 16),
                        w3=int(w3_s, 16),
                        line=line,
                    )
                )
                continue

            match = TRACE_PATTERNS[1].match(line)
            if match:
                words_s, read_lba_s, data_lba_s, hash_s, w0_s, w1_s, w2_s, w3_s = match.groups()
                words = int(words_s)
                lba = int(data_lba_s)
                records.append(
                    TraceRecord(
                        source="DMA3DATA",
                        lba=lba,
                        word_count=words,
                        hash_value=int(hash_s, 16),
                        w0=int(w0_s, 16),
                        w1=int(w1_s, 16),
                        w2=int(w2_s, 16),
                        w3=int(w3_s, 16),
                        line=line,
                    )
                )
    return records


def first_words(data: bytes, word_count: int = 4) -> tuple[int, int, int, int]:
    values = []
    for i in range(word_count):
        offset = i * 4
        if offset + 4 <= len(data):
            values.append(le32(data, offset))
        else:
            values.append(0)
    return values[0], values[1], values[2], values[3]


def build_expected_records(
    sector_iter: Iterable[tuple[int, bytes]],
    sector_size: int,
) -> dict[int, dict[str, ExpectedRecord]]:
    sector_map: dict[int, dict[str, ExpectedRecord]] = {}
    for sector_index, raw in sector_iter:
        parts = sector_payload_slices(raw, sector_size)
        source_map: dict[str, ExpectedRecord] = {}

        for source, data in (
            ("ISORAW", parts["ISORAW"]),
            ("CDUSER", parts["CDUSER"]),
            ("CDFIFO", parts["CDFIFO"]),
            ("DMA3DATA", parts["DMA3HDR"]),
            ("DMA3DATA", parts["DMA3PAYLOAD"]),
        ):
            word_count = len(data) // 4
            w0, w1, w2, w3 = first_words(data)
            source_map[f"{source}:{word_count}"] = ExpectedRecord(
                sector_index=sector_index,
                source=source,
                word_count=word_count,
                hash_value=fnv1a_hash_bytes(data),
                w0=w0,
                w1=w1,
                w2=w2,
                w3=w3,
            )

        sector_map[sector_index] = source_map
    return sector_map


def video_header_key_from_words(w0: int, w1: int, w2: int, w3: int) -> VideoHeaderKey | None:
    if w0 != 0x80010160:
        return None
    return VideoHeaderKey(chunk_field=w1, frame_number=w2, demux_size=w3)


def score_alignment(
    expected_records: dict[int, dict[str, ExpectedRecord]],
    trace_records: list[TraceRecord],
    lba_base: int,
    align_source: str,
) -> tuple[int, int]:
    compared = 0
    matches = 0
    for trace in trace_records:
        if trace.source != align_source:
            continue
        sector_index = trace.lba - lba_base
        expected_by_source = expected_records.get(sector_index)
        if expected_by_source is None:
            continue
        expected = expected_by_source.get(f"{trace.source}:{trace.word_count}")
        if expected is None:
            continue
        compared += 1
        if (
            expected.hash_value == trace.hash_value
            and expected.w0 == trace.w0
            and expected.w1 == trace.w1
            and expected.w2 == trace.w2
            and expected.w3 == trace.w3
        ):
            matches += 1
    return matches, compared


def score_video_header_alignment(
    expected_records: dict[int, dict[str, ExpectedRecord]],
    trace_records: list[TraceRecord],
    lba_base: int,
) -> tuple[int, int]:
    compared = 0
    matches = 0
    for trace in trace_records:
        if trace.source not in ("CDUSER", "CDFIFO", "DMA3DATA"):
            continue
        if trace.source == "DMA3DATA" and trace.word_count != 8:
            continue
        if trace.source in ("CDUSER", "CDFIFO") and trace.word_count < 4:
            continue
        trace_key = video_header_key_from_words(trace.w0, trace.w1, trace.w2, trace.w3)
        if trace_key is None:
            continue
        sector_index = trace.lba - lba_base
        expected_by_source = expected_records.get(sector_index)
        if expected_by_source is None:
            continue
        expected = expected_by_source.get("CDUSER:512")
        if expected is None:
            continue
        expected_key = video_header_key_from_words(
            expected.w0, expected.w1, expected.w2, expected.w3
        )
        if expected_key is None:
            continue
        compared += 1
        if expected_key == trace_key:
            matches += 1
    return matches, compared


def find_best_alignment(
    expected_records: dict[int, dict[str, ExpectedRecord]],
    trace_records: list[TraceRecord],
    start_lba: int,
    align_source: str,
    search_radius: int,
) -> tuple[int, int, int]:
    best_base = start_lba
    best_matches = -1
    best_compared = -1

    for lba_base in range(start_lba - search_radius, start_lba + search_radius + 1):
        matches, compared = score_alignment(expected_records, trace_records, lba_base, align_source)
        if matches > best_matches or (matches == best_matches and compared > best_compared):
            best_base = lba_base
            best_matches = matches
            best_compared = compared

    return best_base, best_matches, best_compared


def find_best_video_header_alignment(
    expected_records: dict[int, dict[str, ExpectedRecord]],
    trace_records: list[TraceRecord],
    start_lba: int,
    search_radius: int,
) -> tuple[int, int, int]:
    best_base = start_lba
    best_matches = -1
    best_compared = -1

    for lba_base in range(start_lba - search_radius, start_lba + search_radius + 1):
        matches, compared = score_video_header_alignment(
            expected_records,
            trace_records,
            lba_base,
        )
        if matches > best_matches or (matches == best_matches and compared > best_compared):
            best_base = lba_base
            best_matches = matches
            best_compared = compared

    return best_base, best_matches, best_compared


def compare_trace(
    expected_records: dict[int, dict[str, ExpectedRecord]],
    trace_path: Path,
    lba_base: int,
) -> None:
    trace_records = parse_trace_records(trace_path)
    checked = 0
    mismatches = 0

    print("Trace Compare")
    print(f"- trace: {trace_path}")
    print(f"- lba base: {lba_base}")

    for record in trace_records:
        sector_index = record.lba - lba_base
        expected_by_source = expected_records.get(sector_index)
        if expected_by_source is None:
            continue

        expected = expected_by_source.get(f"{record.source}:{record.word_count}")
        if expected is None:
            continue

        checked += 1
        ok = (
            expected.hash_value == record.hash_value
            and expected.w0 == record.w0
            and expected.w1 == record.w1
            and expected.w2 == record.w2
            and expected.w3 == record.w3
        )
        status = "OK" if ok else "MISMATCH"
        if not ok:
            mismatches += 1
        print(
            f"  {status:8s} {record.source:7s} lba={record.lba:4d} words={record.word_count:4d} "
            f"log_hash=0x{record.hash_value:08X} exp_hash=0x{expected.hash_value:08X} "
            f"log_w0=0x{record.w0:08X} exp_w0=0x{expected.w0:08X}"
        )
        if not ok:
            print(f"    trace: {record.line}")
            print(
                f"    expected words: "
                f"0x{expected.w0:08X} 0x{expected.w1:08X} 0x{expected.w2:08X} 0x{expected.w3:08X}"
            )

    print(f"- compared entries: {checked}")
    print(f"- mismatches: {mismatches}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="Path to raw .STR / sector file")
    parser.add_argument("--sector-size", type=int, choices=(2336, 2352), help="Override sector size")
    parser.add_argument("--iso-file", help="When PATH is a .cue, resolve this ISO9660 file path from the disc")
    parser.add_argument("--start", type=int, default=0, help="Start sector index")
    parser.add_argument("--count", type=int, help="Number of sectors to dump")
    parser.add_argument("--summary-only", action="store_true", help="Skip per-sector table")
    parser.add_argument("--csv", type=Path, help="Optional CSV output path")
    parser.add_argument("--compare-trace", type=Path, help="Compare STR contents against a timeline trace")
    parser.add_argument("--lba-base", type=int, default=0, help="LBA corresponding to STR sector 0 when using --compare-trace")
    parser.add_argument(
        "--auto-align",
        action="store_true",
        help="Auto-detect the best LBA base against --compare-trace before comparing",
    )
    parser.add_argument(
        "--align-source",
        choices=("ISORAW", "CDUSER", "CDFIFO", "DMA3DATA", "VIDEOHDR"),
        default="CDUSER",
        help="Trace source to use when searching the best alignment",
    )
    parser.add_argument(
        "--align-radius",
        type=int,
        default=256,
        help="Search radius around --lba-base when using --auto-align",
    )
    args = parser.parse_args()

    lba_base = 0
    if args.path.suffix.lower() == ".cue":
        if not args.iso_file:
            raise SystemExit("When PATH is a .cue, --iso-file is required")
        sector_size = 2352
        raw_records = list(iter_cue_file_records(args.path, args.iso_file, args.start, args.count))
        records = [parse_sector(raw, sector_index, sector_size) for sector_index, raw in raw_records]
        # Retrieve file LBA for absolute sector addressing in summary
        _, lba_base, _ = resolve_iso_file_from_cue(args.path, args.iso_file)
    else:
        sector_size = detect_sector_size(args.path, args.sector_size)
        records = list(iter_sector_records(args.path, sector_size, args.start, args.count))

    if not args.summary_only:
        print_sector_table(records)
    print_summary(args.path, sector_size, records, lba_base=lba_base)

    if args.csv:
        write_csv(args.csv, records)
        print(f"- csv: {args.csv}")

    if args.compare_trace:
        print()
        if args.path.suffix.lower() == ".cue":
            raw_records = list(iter_cue_file_records(args.path, args.iso_file, args.start, args.count))
        else:
            raw_records = list(iter_sector_bytes(args.path, sector_size, args.start, args.count))

        expected_records = build_expected_records(raw_records, sector_size)
        lba_base = args.lba_base
        if args.auto_align:
            trace_records = parse_trace_records(args.compare_trace)
            if args.align_source == "VIDEOHDR":
                lba_base, matches, compared = find_best_video_header_alignment(
                    expected_records,
                    trace_records,
                    args.lba_base,
                    args.align_radius,
                )
            else:
                lba_base, matches, compared = find_best_alignment(
                    expected_records,
                    trace_records,
                    args.lba_base,
                    args.align_source,
                    args.align_radius,
                )
            print("Auto Align")
            print(f"- source: {args.align_source}")
            print(f"- best lba base: {lba_base}")
            print(f"- matches: {matches}")
            print(f"- compared: {compared}")
            print()

        compare_trace(expected_records, args.compare_trace, lba_base)


if __name__ == "__main__":
    main()
