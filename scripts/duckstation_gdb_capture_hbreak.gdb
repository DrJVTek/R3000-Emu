set pagination off
set confirm off
set architecture mips
set endian little

target remote 127.0.0.1:2345

set logging file e:/Projects/github/Live/R3000-Emu/logs/duckstation_gdb.txt
set logging overwrite on
set logging enabled on

set $hits = 0

define dump_psx_state
  printf "=== STOP PC=0x%08x ===\n", $pc
  info registers
  printf "--- code @PC ---\n"
  x/32wx $pc
  printf "\n"
end

define hook-stop
  set $hits = $hits + 1
  dump_psx_state
  if $hits >= 4
    printf "=== DONE (hits=%d) ===\n", $hits
    set logging enabled off
    quit
  end
  continue
end

hbreak *0x8005ee80
hbreak *0x8005ef30
hbreak *0x80067938
hbreak *0x8006797c

continue
