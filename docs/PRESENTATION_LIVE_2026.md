# Live Coding — R3000-Emu : Émulateur PS1 + Analyse IA

> **Présentation live — 2026-04-18**
> Sujet : construire un émulateur PlayStation 1 open-source, y brancher des LLMs via MCP
> pour analyser automatiquement le rendu 3D, et faire jouer un LLM à un jeu 2D PS1.

---

## 1. Le contexte — Pourquoi un émulateur PS1 maison ?

### Le projet PSX-VR

L'objectif long terme est de **rejouer des jeux PS1 en réalité virtuelle sur Meta Quest 3**.
Le problème fondamental : la PlayStation 1 n'a **pas de GPU 3D**.

Sur PS1, voici ce qui se passe dans chaque frame :

```
Jeu (MIPS R3000A)
    ↓
GTE (COP2) — Geometry Transform Engine
    ↓  projette les vertices 3D → coordonnées écran 2D
    ↓  calcule depth, normals, lighting
    ↓
Ordering Table (OT) — liste chainée triée par Z
    ↓
DMA2 → GPU (GP0) — rasterise des polygones 2D
    ↓
VRAM → écran
```

**À la sortie du pipeline, la 3D originale est perdue.** Le GPU ne voit que des `POLY_GT4`
avec des coordonnées X/Y en pixels. Les matrices de rotation, les vertices 3D, la hiérarchie
des objets — tout ça reste dans le code MIPS, invisible au GPU.

Pour faire du VR il faut **intercepter le GTE** : c'est là que les données 3D vivent.

### Pourquoi refaire un émulateur ?

Les émulateurs existants (DuckStation, PCSX-R, Mednafen) sont excellents pour jouer
**mais pas pour analyser**. Ils n'exposent pas :

- Les matrices GTE instruction par instruction
- L'association vertex → polygon → OT bucket
- L'identité des objets 3D dans la scène
- Un protocole pour qu'un LLM puisse interagir en temps réel

R3000-Emu est conçu dès le départ comme **un outil d'analyse**, pas juste un lecteur de jeux.

---

## 2. L'architecture de l'émulateur

### Composants émulés

```
r3000_emu.exe
├── CPU  — MIPS R3000A (33.8 MHz)
│   ├── Pipeline entier : fetch / decode / execute / writeback
│   ├── Exceptions (IRQ, syscall, TLB miss)
│   └── COP0 (status, cause, EPC...)
├── GTE  — COP2, Geometry Transform Engine
│   ├── RTPS / RTPT  — projection perspective (1 ou 3 vertices)
│   ├── NCLIP        — back-face culling
│   ├── NCS/NCT/NCDS — lighting normal-based
│   ├── AVSZ3/AVSZ4  — average Z pour OT bucket
│   └── MTC2/MFC2/CTC2 — accès aux registres GTE
├── GPU  — Rasterizer 2D
│   ├── Parsing des GP0 packets (POLY_F3/G4/FT4/GT4...)
│   ├── Draw List tracking (origin 2D/3D, textured, semi-transparent)
│   └── Corrélation GTE↔GPU (quel RTPS a produit quel polygon ?)
├── CDROM — Controller + SBI/LibCrypt
├── SPU   — Sound Processing Unit
├── DMA   — Channels 0-6 + DMA2 (GPU linked-list)
└── BIOS  — SCPH-compatible, pas de fast-boot (mode réaliste)
```

### Ce qui rend l'émulateur unique : le GTE Snapshot

À chaque instruction GTE, l'émulateur capture :

```cpp
struct GteEvent {
    uint32_t  pc;          // adresse MIPS de l'instruction
    uint32_t  opcode;      // RTPS/RTPT/NCLIP/etc.
    GteRegs   regs_before; // matrice RT, TR, SXY0-2, SZ0-3...
    GteRegs   regs_after;
    uint32_t  frame_id;
};
```

Ces événements sont corrélés avec les packets GPU pour relier :
`PC qui a fait RTPS` → `vertex projeté` → `polygon dans l'OT` → `triangle dans la VRAM`

C'est ce **double-buffer de corrélation** qui permet la reconstruction 3D.

---

## 3. Le protocole MCP — l'émulateur comme toolbox pour LLMs

### Qu'est-ce que MCP ?

MCP (Model Context Protocol) est le protocole standard d'Anthropic pour connecter
des LLMs à des outils externes. C'est du JSON-RPC sur stdio ou HTTP.

```json
// Requête (Python → emu)
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"emu.resume","arguments":{"max_steps":50000,"max_frames":1}}}

// Réponse (emu → Python)
{"jsonrpc":"2.0","id":1,
 "result":{"content":[{"type":"text","text":"{\"steps_done\":50000,\"frames\":1}"}]}}
```

### L'émulateur comme serveur MCP stdio

```
python classifier.py  ←→  r3000_emu.exe --mcp-stdio  ←→  BIOS + ROM
```

Le launcher Python démarre l'émulateur en subprocess, échange des messages MCP
sur stdin/stdout. Depuis Python (ou n'importe quel LLM client MCP), on peut :

| Outil MCP | Ce qu'il fait |
|---|---|
| `emu.resume` | Avance N steps ou M frames |
| `emu.get_cpu_state` | Lit PC, registres, mode |
| `emu.set_gte_trace_window` | Active la capture GTE sur un range PC |
| `emu.get_gte_trace_summary` | Lit les top PCs qui ont exécuté du GTE |
| `emu.get_draw_list_summary` | Résumé de la draw list courante (2D/3D counts) |
| `emu.get_dma2_nohint_summary` | Hotspot PCs du DMA2 GPU linked-list |
| `emu.step_with_pad_observation` | Appuie sur un bouton + observe la scène |
| `emu.match_render_pattern` | Fingerprint complet pour le LLM classifier |
| `emu.set_breakpoint_pc` | Pose un breakpoint CPU |
| `emu.add_mem_watch_write` | Watchpoint RAM (quelle adresse écrit quel PC) |

### Pourquoi MCP et pas une API custom ?

- **Composabilité** : un LLM Claude peut piloter directement l'émulateur sans couche intermédiaire
- **Introspection** : `tools/list` expose les schémas JSON → l'IA sait ce qu'elle peut faire
- **Standard** : n'importe quel client MCP (Claude, GPT-4, Cursor, IDE) peut s'y connecter
- **Traçabilité** : chaque appel est loggé en JSONL → replay et debug facile

---

## 4. Le GTE Trap — comment trouver le code 3D automatiquement

### Le problème

Un jeu PS1 peut avoir 500 000 fonctions MIPS. La plupart sont de la logique de jeu,
de l'audio, de la gestion CD. On veut trouver **les 5-15 fonctions qui font du rendu 3D**.

### La solution : GTE trace window

```python
# 1. Activer la capture passive sur toute la RAM PSX
emu.set_gte_trace_window(pc_start=0, pc_end=0, enabled=True)

# 2. Avancer le jeu (frames + boutons pour déclencher la 3D)
emu.resume(max_steps=2_000_000, max_frames=30)

# 3. Lire les PCs qui ont exécuté des instructions GTE structurelles
summary = emu.get_gte_trace_summary()
# → {"top_pcs": [{"pc": 2148888324, "count": 18240}, ...], "top_ops": [...]}
```

Les PCs avec le plus d'appels GTE sont **exactement** les fonctions de rendu 3D.
On peut ensuite les passer à Ghidra via MCP pour obtenir les noms de fonctions.

### Phase p3b — GTE trap pour les jeux avec intro 2D

Certains jeux (T-Rex, Crash Bandicoot intro...) ont 200+ frames de 2D avant la 3D.
Le seek loop standard ne trouve rien. La solution :

```
while frames < 300:
    emu.step_with_pad_observation(names_csv="start", hold_steps=1, observe_steps=20000)
    summary = emu.get_gte_trace_summary()
    if pcs_found_in_psx_ram_range:
        break  # trouvé !
```

Injection de boutons (START, CROSS, TRIANGLE...) pour passer les menus et atteindre la 3D.

### Phase p3c — GTE trap BIOS (le logo PlayStation)

Le logo PlayStation tournant est lui-même rendu avec GTE (RTPS + NCLIP) depuis le BIOS ROM.
Capturer ces PCs permet d'identifier les fonctions de rendu du BIOS — utile pour comprendre
si le jeu appelle des routines BIOS pour son rendu (certains jeux le font).

```python
# Trace globale (pas de filtre PC) → capte BIOS KSEG0 et KSEG1
emu.set_gte_trace_window(pc_start=0, pc_end=0, enabled=True)
emu.resume(max_steps=100_000_000, max_frames=0)
summary = emu.get_gte_trace_summary()
# Filtre Python pour 0x9FC00000-0x9FC80000 et 0xBFC00000-0xBFC80000
```

---

## 5. Le pipeline d'analyse LLM (le classifier)

### Architecture générale

```
scripts/classifier/
├── __main__.py          — CLI + orchestrateur principal
├── orchestrator.py      — Coordonne toutes les phases
├── config.py / yaml     — Configuration (emu, ghidra, llm, workflow)
├── phases/
│   ├── p1_boot.py       — Boot contrôlé (paused / pause_immediate)
│   ├── p2_gte_discovery — Analyse statique Ghidra
│   ├── p3_loop_discovery— Analyse dynamique runtime
│   ├── p3b_gte_trap.py  — GTE trap jeux 2D
│   ├── p3c_bios_gte_trap— GTE trap BIOS logo
│   ├── p4_classify.py   — Classification LLM
│   └── p5_profile.py    — Génération .psx3dprof
└── mcp/
    ├── emu_stdio.py     — Client MCP natif stdio
    ├── ghidra_http.py   — Client GhidraMCP HTTP
    └── llm_litellm.py   — Client LLM (DashScope, OpenRouter, Ollama...)
```

### Le flux complet

```
         pause_immediate
              │
         [p1_boot]          PC = 0xBFC00000 (BIOS reset vector)
              │
         [p3c]              GTE trap BIOS → PCs logo PlayStation
              │
         [p2_gte_discovery] Ghidra MCP → fonctions statiques + xrefs GTE
              │
         [_maybe_resume]    Advance jusqu'au runtime du jeu
              │
         [p3_loop_discovery]
              │  readiness score : origin_3d, texturés, GTE ops, DMA2 hotspots
              │
              ├─ score < 4.0 → [p3b_gte_trap] injecteur pad 300 frames
              │
         [p4_classify]      LLM + playbook → Type A-F + ModeKind
              │
         [p5_profile]       .psx3dprof (ranges PC, mode, paramètres)
```

### Le playbook des types de rendu (A-F)

Le LLM reçoit en system prompt la **taxonomie complète** des 6 types de boucle de rendu PS1 :

| Type | Nom technique | Description | Exemples |
|---|---|---|---|
| **A** | `ot_classic_rtpt` | OT + RTPT + AVSZ3/4. Le plus courant (~65%). | Ridge Racer, TREX |
| **B** | `paired_edge_rtpt_gt4` | Quads par paires de triangles. | Metal Gear Solid |
| **C** | `flat_z_sort_rtps` | RTPS sur chaque vertex, Z-sort manuel. | RPGs 2D/3D mixtes |
| **D** | `sprite_ot_rtps` | Sprites 2D mélangés dans l'OT. | Jeux 2D avec layers |
| **E** | `subdivided_ft4_intpl_rtpt` | Subdivision + interpolation. Terrain. | Wipeout, Crash |
| **F** | `custom_engine` | Moteur maison sans PSYQ standard. | Quake, Duke3D ports |

Le LLM reçoit le fingerprint JSON de `match_render_pattern` et applique l'arbre de décision
pour produire une classification structurée avec confidence score.

### Le résultat : fichier .psx3dprof

```ini
# Ridge Racer (USA).psx3dprof
MODE ot_classic_rtpt
GTE_CALLERS 0x8002A1C4-0x8002A200 0x80031000-0x80031080
OT_CALLERS  0x80028000-0x80028100
```

Ce fichier alimente le composant UE5 qui lit le GTE en temps réel et reconstruit la 3D.

---

## 6. L'intégration Unreal Engine 5

### Architecture UE5

```
UE5 Plugin (PSX3D)
├── PSX2DRenderComponent    — Affiche la VRAM 2D (le rendu PS1 original)
├── PSX3DRenderComponent    — Maillage 3D reconstruit en temps réel
│   ├── Lit le .psx3dprof
│   ├── Accroche les callbacks GTE (RTPS/RTPT events)
│   └── Reconstruit position + rotation de chaque objet à chaque frame
└── VideoComponent          — Lecture FMV (STR streaming MDEC)
```

### Compilation directe (pas de DLL)

Le plugin UE5 compile directement les sources C++ de l'émulateur via un symlink :
```
PrivateSources/src → repo/src/
```
UBT découvre tous les .cpp récursivement. Live Coding fonctionne (Ctrl+Alt+F11).

---

## 7. Ce qu'on va coder en live ce soir

### Agenda proposé

1. **Tour de l'architecture** (10 min)
   - Montrer le repo, la structure, lancer `r3000_emu.exe --help`
   - Lancer un jeu en mode normal, voir le rendu 2D dans l'écran de débogage

2. **Le GTE trap en direct** (15 min)
   - Lancer le classifier sur Ridge Racer (ou TREX)
   - Observer les logs JSONL en temps réel
   - Voir les PCs GTE apparaître, les corréler dans Ghidra

3. **L'analyse LLM** (10 min)
   - Voir le fingerprint JSON généré par `match_render_pattern`
   - Observer le LLM lire le fingerprint et classifier (Type A, 0.85 confidence)
   - Voir le .psx3dprof généré

4. **La démo fun : LLM joue à un jeu PS1** (15-20 min)
   - Charger un jeu 2D simple
   - Boucle Python simple : observer → LLM décide → appuyer bouton → répéter
   - Voir le LLM "apprendre" à naviguer dans le jeu

### Commandes utiles pour le live

```bash
# Lancer l'émulateur en mode MCP stdio
./lib/Release/r3000_emu.exe --mcp-stdio --bios=bios/ps1_bios.bin --cd="jeu.cue" --log-level=info

# Lancer le classifier complet
python -m scripts.classifier --game RIDGERACER --cd "Ridge Racer (USA).cue"

# Classifier en mode bios-only (tester le BIOS logo GTE)
python -m scripts.classifier --game BIOS-LOGO --bios-only --launch-mode pause_immediate

# Voir les logs en temps réel (autre terminal)
tail -f logs/classifier/RIDGERACER-*.jsonl | python -m json.tool
```

---

## 8. Ce qui a été construit (récapitulatif technique)

### Côté C++ (l'émulateur)

- **CPU R3000A complet** : ~4500 lignes, pipeline entier, exceptions, COP0
- **GTE COP2 complet** : tous les opcodes, registres shadow, flags
- **GPU** : GP0/GP1, tous les types de polygones, draw list avec corrélation GTE
- **CDROM** : SBI/LibCrypt, XA-ADPCM, mode 2 XA, annulation de read en cours
- **DMA** : channels 0-6, DMA2 linked-list GPU
- **MCP Server** : 40+ outils JSON-RPC sur stdio
- **GTE Trace** : capture passive par PC range, histogramme top_pcs/top_ops
- **Watchpoints RAM** : filtres PC + valeur, ring buffer d'événements
- **Breakpoints PC** : managed breakpoints via MCP
- **Scene vector** : corrélation GTE↔GPU → groupes 3D + centres + salience

### Côté Python (le classifier)

- **6 phases** : boot → static → seek → GTE trap → LLM classify → profile
- **3 clients MCP** : emu_stdio, ghidra_http, llm_litellm
- **Taxonomie A-F** avec cross-validation enum C++
- **Narrator TTS** : commentaire vocal pendant l'analyse
- **Rapport Markdown** + log JSONL structuré par session
- **Config YAML** + env vars + CLI args (précédence en couches)
- **GTE trap automatique** : détecte intro 2D et injecte des boutons pad

### Lignes de code (approximatif)

| Composant | ~LOC |
|---|---|
| CPU R3000A | 4 500 |
| GTE COP2 | 2 000 |
| GPU | 3 000 |
| CDROM + SPU | 2 500 |
| MCP Server | 2 000 |
| Python classifier | 3 500 |
| UE5 Plugin | 5 000 |
| **Total** | **~22 500** |
