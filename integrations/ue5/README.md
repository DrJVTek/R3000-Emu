# UE5 Integration (R3000-Emu)

Objectif: intégrer le **core** de R3000-Emu dans un projet **Unreal Engine 5** tout en gardant la **CLI** comme harness de test.

## Scope

- **CLI**: harness (CPU/bus/CDROM/logs/trace). Pas d’objectif “rendu GPU” ni “audio SPU”.
- **UE5**: intégration runtime + observabilité UE-friendly (Output Log, paths configurables).

## Build / Test (smoke)

Voir `specs/001-ue5-integration/quickstart.md`.

## Plugin UE5

Le plugin est dans `integrations/ue5/R3000Emu/` (fichier `.uplugin`: `R3000Emu.uplugin`).

### Notes importantes

- Ce plugin compile le core **directement depuis** les sources du repo (via includes relatifs dans `R3000CoreFromRepo.cpp`).
- Donc il est prévu pour être utilisé en gardant **ce repo** tel quel (ex: repo cloné ou submodule dans ton projet UE).

