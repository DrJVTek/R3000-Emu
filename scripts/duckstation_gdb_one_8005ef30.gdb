set pagination off
set confirm off
set architecture mips
set endian little

target remote 127.0.0.1:2345

set logging file e:/Projects/github/Live/R3000-Emu/logs/duckstation_8005ef30.txt
set logging overwrite on
set logging enabled on

define dump_state
  printf "=== HIT PC=0x%08x ===\n", $pc
  info registers
  printf "--- code @PC ---\n"
  x/32wx $pc
  printf "\n"
end

set $target = 0x8005ef30
hbreak *0x8005ef30

define hook-stop
  if $pc == $target
    dump_state
    quit
  end
  continue
end

continue
