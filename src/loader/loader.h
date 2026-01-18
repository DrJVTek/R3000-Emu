#pragma once

#include <cstddef>
#include <cstdint>

namespace loader
{

// Résultat d'un chargement de binaire (PS-X EXE / ELF).
struct LoadedImage
{
    uint32_t entry_pc;
    uint32_t gp;
    uint32_t sp;

    int has_gp;
    int has_sp;
};

enum class Format
{
    auto_detect,
    psxexe,
    elf,
};

// Charge un fichier (path) en RAM et retourne l'entry point.
// - ram = mémoire physique (0..ram_size-1)
// - err = buffer d'erreur (optionnel)
int load_file_into_ram(
    const char* path,
    Format fmt,
    uint8_t* ram,
    size_t ram_size,
    LoadedImage* out,
    char* err,
    size_t err_cap
);

} // namespace loader

