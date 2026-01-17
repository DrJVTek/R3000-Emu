#pragma once

#include <cstdint>

// GTE (Geometry Transformation Engine) de la PS1.
// Objectif: garder tout le code GTE séparé du CPU pour une base propre et pédagogique.
//
// Dans l'ISA, le GTE est exposé via COP2:
// - MFC2/MTC2 : transferts avec les registres "data" du GTE
// - CFC2/CTC2 : transferts avec les registres "control" du GTE
// - Instructions GTE (RTPS, MVMVA, NCLIP, etc.) via le champ "function" de COP2
namespace gte
{

class Gte
{
  public:
    Gte();

    void reset();

    // Registres data/control (index 0..31).
    // NOTE: certains registres sont packés/saturés en vrai. On commence simple: stockage brut
    // 32-bit.
    uint32_t read_data(uint32_t idx) const;
    void write_data(uint32_t idx, uint32_t v);

    uint32_t read_ctrl(uint32_t idx) const;
    void write_ctrl(uint32_t idx, uint32_t v);

    // Plus tard: exécution des commandes GTE.
    // void execute(uint32_t command_word);

  private:
    uint32_t data_[32]{};
    uint32_t ctrl_[32]{};
};

} // namespace gte
