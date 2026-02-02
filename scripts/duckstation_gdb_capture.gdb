set pagination off
set architecture mips
set endian little

target remote 127.0.0.1:2345

set logging file e:/Projects/github/Live/R3000-Emu/logs/duckstation_gdb.txt
set logging overwrite on
set logging on

define dump_psx_state
  printf "=== HIT PC=0x%08x ===\n", $pc
  info registers
  printf "--- MMIO (KSEG0) ---\n"
  x/wx 0x1f801070
  x/wx 0x1f801074
  x/wx 0x1f8010f0
  x/wx 0x1f8010f4
  x/wx 0x1f801814
  x/wx 0x1f801100
  x/wx 0x1f801104
  x/wx 0x1f801108
  x/wx 0x1f801110
  x/wx 0x1f801114
  x/wx 0x1f801118
  x/wx 0x1f801120
  x/wx 0x1f801124
  x/wx 0x1f801128
  printf "--- MMIO (KSEG1 alias) ---\n"
  x/wx 0xbf801070
  x/wx 0xbf801074
  x/wx 0xbf8010f0
  x/wx 0xbf8010f4
  x/wx 0xbf801814
  printf "--- code @PC ---\n"
  x/32wx $pc
  printf "\n"
end

break *0x8005ee80
commands
  dump_psx_state
  continue
end

break *0x8005ef30
commands
  dump_psx_state
  continue
end

break *0x80067938
commands
  dump_psx_state
  continue
end

break *0x8006797c
commands
  dump_psx_state
  continue
end

continue
