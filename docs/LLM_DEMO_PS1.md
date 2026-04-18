# Démo Live — Un LLM joue à un jeu PS1 2D

> **Pour la présentation live — 2026-04-18**
> Guide technique pour faire jouer un LLM à un jeu PlayStation 1 2D
> via le protocole MCP stdio de R3000-Emu.

---

## Concept

L'outil `emu.step_with_pad_observation` fait trois choses en un seul appel MCP :

1. **Appuie** sur un ou plusieurs boutons de la manette (hold N steps)
2. **Avance** l'émulateur de M steps supplémentaires pour observer
3. **Retourne** un snapshot JSON de la scène courante

Le LLM reçoit ce snapshot, décide quelle action faire, et la boucle recommence.
C'est exactement une boucle **observe → think → act** — le pattern agent de base.

```
┌─────────────────────────────────────────────────────┐
│                   BOUCLE AGENT LLM                  │
│                                                     │
│  emu.step_with_pad_observation                      │
│       ↓                                             │
│  scene_snapshot + scene_delta + scene_salience      │
│       ↓                                             │
│  LLM (Claude / Qwen / GPT-4...)                    │
│  "Vu l'état de la scène, j'appuie sur CROSS"       │
│       ↓                                             │
│  → bouton parsé → prochain appel MCP               │
└─────────────────────────────────────────────────────┘
```

---

## Ce que le LLM reçoit : le scene snapshot

Exemple de retour de `step_with_pad_observation` :

```json
{
  "pad_action": {"pressed": ["cross"], "held_steps": 1},
  "observe_steps": 20000,
  "scene_snapshot": {
    "frame_id": 142,
    "dominant_kind": "2d_sprite",
    "group_count": 8,
    "groups": [
      {
        "id": 0,
        "centroid_x": 0.52, "centroid_y": 0.38,
        "poly_count": 12,
        "kind": "textured_2d",
        "ot_depth_mean": 128
      }
    ]
  },
  "scene_delta": {
    "group_count_delta": 0,
    "centroid_shift_max": 0.12,
    "appeared": [],
    "disappeared": [],
    "moved_groups": [{"id": 0, "dx": 0.05, "dy": 0.0}]
  },
  "scene_salience": {
    "most_active_group": 0,
    "salience_score": 0.72
  },
  "focus_candidate": {
    "group_id": 0,
    "confidence": 0.80,
    "reason": "largest_mover"
  }
}
```

Le LLM interprète cela sans voir les pixels. Il voit :
- Combien de groupes d'objets sont à l'écran
- Leur position (coordonnées normalisées 0-1)
- Leur mouvement depuis le dernier step (delta)
- Quel objet bouge le plus (salience → probablement le personnage)

---

## Choix du jeu pour la démo

### Critères

- **2D simple** : pas de 3D complexe, la scène est lisible par le LLM
- **Réponse rapide aux inputs** : le personnage bouge visiblement à chaque bouton
- **Pas de chargement CD long** : évite l'attente pendant le live
- **ROM légale ou homebrew** : important pour le stream

### Options recommandées

| Jeu | Raison |
|---|---|
| **Homebrew PS1 simple** | Légal à distribuer, simple par design |
| **Tetris (si disponible)** | Pièces = groupes distincts, objectif clair |
| **Jeu de plateforme simple** | Saut visible dans le delta de position |
| **Pong / Breakout clone** | Paddle = centroid qui bouge, ball = groupe salient |

### Alternative : créer un mini-jeu de test

Un ROM homebrew minimaliste peut être compilé avec `mipsel-none-elf-gcc` :

```c
// mini_game.c — carré qui bouge avec les boutons
// Compilé avec le toolchain MIPS inclus dans le repo (voir README)
```

---

## Le code de la démo

### llm_player.py

```python
"""LLM PS1 Player — fait jouer un LLM à un jeu PS1 via MCP stdio."""
import json
import sys
from scripts.classifier.mcp.emu_stdio import EmuMcp
from scripts.classifier.mcp.llm_litellm import LlmClient
from scripts.classifier import config as cfg_mod

SYSTEM_PROMPT = """
Tu pilotes un jeu PlayStation 1 via des boutons de manette.
À chaque tour, tu reçois un snapshot JSON de la scène courante.

Règles :
- Analyse les groupes de polygones : position, mouvement, taille
- Le groupe avec le score de salience le plus haut est probablement ton personnage
- Choisis UN seul bouton parmi : up, down, left, right, cross, circle, triangle, square, start
- Réponds UNIQUEMENT avec le nom du bouton, rien d'autre.

Objectif : explore le jeu, essaie de progresser, évite les obstacles.
"""

BUTTONS = ["up", "down", "left", "right", "cross", "circle", "triangle", "square", "start"]

def parse_button(text: str) -> str:
    text = text.strip().lower()
    for btn in BUTTONS:
        if btn in text:
            return btn
    return "cross"  # fallback

def run_llm_player(game_cue: str, max_turns: int = 200):
    cfg = cfg_mod.load()
    llm = LlmClient(cfg.llm)

    with EmuMcp(
        emu_exe=cfg.emu.exe,
        bios=cfg.emu.bios,
        rom_args=[f"--cd={game_cue}"],
    ) as emu:
        emu.initialize(client_name="llm_player")

        # Boot jusqu'au jeu
        print("Boot en cours...")
        emu.call_tool_json("emu.resume", {"max_steps": 5_000_000, "max_frames": 300})
        print("Jeu chargé. Début de la boucle LLM.\n")

        history = []
        for turn in range(max_turns):
            # 1. Observer la scène (appuyer sur le dernier bouton choisi)
            button = history[-1] if history else "start"
            state = emu.call_tool_json("emu.step_with_pad_observation", {
                "names_csv": button,
                "hold_steps": 3,
                "observe_steps": 15000,
                "max_groups": 12,
                "max_targets": 6,
            })

            # 2. Préparer le contexte pour le LLM
            snapshot = json.dumps(state, indent=2)
            user_msg = f"Tour {turn + 1}. État de la scène :\n{snapshot}\n\nQuel bouton appuyer ?"

            # 3. Appel LLM
            response = llm.complete(
                system=SYSTEM_PROMPT,
                messages=[{"role": "user", "content": user_msg}],
            )
            button_choice = parse_button(response)
            history.append(button_choice)

            # 4. Log pour le live
            delta = state.get("scene_delta", {})
            salience = state.get("scene_salience", {})
            print(
                f"[{turn+1:3d}] bouton={button_choice:8s} | "
                f"groupes={state.get('scene_snapshot',{}).get('group_count',0)} | "
                f"shift_max={delta.get('centroid_shift_max',0):.3f} | "
                f"salience={salience.get('salience_score',0):.2f}"
            )

if __name__ == "__main__":
    game_cue = sys.argv[1] if len(sys.argv) > 1 else "game.cue"
    run_llm_player(game_cue)
```

### Lancer la démo

```bash
# Depuis la racine du repo
python scripts/llm_player.py "chemin/vers/jeu.cue"

# Avec un modèle local (Ollama)
CLASSIFIER_LLM_PROVIDER=ollama \
CLASSIFIER_LLM_MODEL=llama3.2 \
python scripts/llm_player.py "jeu.cue"

# Avec Claude (via Anthropic API)
ANTHROPIC_API_KEY=sk-ant-... \
CLASSIFIER_LLM_PROVIDER=anthropic \
CLASSIFIER_LLM_MODEL=claude-haiku-4-5-20251001 \
python scripts/llm_player.py "jeu.cue"
```

---

## Points à montrer pendant la démo

### 1. Le LLM voit la scène sans pixels

Montrer côte à côte :
- L'écran du jeu (window classique)
- Le JSON de scene_snapshot

Le LLM ne voit que des nombres. Il déduit le jeu à partir des mouvements de centroïdes.

### 2. Le scene_delta révèle le mouvement

Quand le joueur marche à droite : `centroid_x` augmente, `centroid_shift_max > 0`.
Quand le joueur saute : `centroid_y` change brusquement.

Le LLM peut inférer "je me suis déplacé vers la droite" sans jamais voir l'image.

### 3. La salience comme guide

`focus_candidate.group_id` identifie le groupe qui bouge le plus = probablement le joueur.
Le LLM peut utiliser ça comme "je dois déplacer ce groupe vers l'objectif".

### 4. Comparaison de LLMs

Essayer le même jeu avec :
- Un petit modèle rapide (Haiku 4.5, ~150ms/appel)
- Un grand modèle (Claude Sonnet, ~500ms/appel)
- Un modèle local (Ollama Llama 3.2)

Observer la différence de stratégie.

---

## Idée d'extension : LLM qui commente sa stratégie

Modifier `SYSTEM_PROMPT` pour demander au LLM d'expliquer son raisonnement :

```
Réponds en deux parties :
RAISON: [une ligne expliquant ton analyse]
BOUTON: [le bouton choisi]
```

Pendant le live, on voit le raisonnement du LLM en direct — plus engaging pour le public.

---

## Questions fréquentes du live

**Q : Pourquoi pas simplement passer les pixels au LLM ?**
R : Les LLMs vision coûtent cher, sont lents, et ne capturent pas le mouvement inter-frame.
Le scene snapshot est léger (~500 bytes JSON), rapide, et contient exactement l'information
dont le LLM a besoin : position + mouvement + salience.

**Q : Est-ce que ça marche vraiment pour jouer ?**
R : Ça dépend du jeu. Pour un jeu très simple (plateformer basique, pong), oui.
Pour un jeu complexe, non — mais ce n'est pas le but. Le but est de montrer la boucle
d'interaction LLM ↔ émulateur via MCP.

**Q : Et pour l'analyse 3D, ça donne quoi en vrai ?**
R : Le classifier a été validé sur TREX (classification `ot_classic_rtpt`, confidence 0.50)
et génère des fichiers .psx3dprof exploitables. Ridge Racer est le prochain test cible.

**Q : La reconstruction 3D marche dans UE5 ?**
R : Oui pour les scènes simples. Le double-buffer GTE↔GPU reconstruit les positions
object-space en temps réel. Les bugs restants sont dans le composant UE5 (near-clip,
degenerate quads) — pas dans l'émulateur lui-même.

---

## Setup complet pour le live

### Prérequis

```bash
# Python deps
pip install pyyaml litellm

# Compiler l'émulateur (Release)
cmake --build build --config Release
# → lib/Release/r3000_emu.exe

# BIOS PS1 (SCPH-compatible, ~512KB)
# → bios/ps1_bios.bin

# ROM du jeu de démo
# → chemin/vers/jeu.cue + jeu.bin
```

### Vérification rapide

```bash
# L'ému répond bien en mode MCP ?
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | \
  ./lib/Release/r3000_emu.exe --mcp-stdio --bios=bios/ps1_bios.bin 2>/dev/null | \
  python -c "import sys,json; d=json.load(sys.stdin); print(len(d['result']['tools']), 'tools')"
# → 40+ tools

# Le classifier démarre ?
python -m scripts.classifier --help

# Le LLM répond ?
python -c "
from scripts.classifier import config; from scripts.classifier.mcp.llm_litellm import LlmClient
cfg = config.load(); llm = LlmClient(cfg.llm)
print(llm.complete('Test', [{'role':'user','content':'dis juste OK'}]))
"
```

### Fichier llm_player.py à placer

Copier le code de la section ci-dessus dans `scripts/llm_player.py`.

---

## Idées bonus si le live se passe bien

1. **Mode compétition** : deux LLMs jouent en parallèle (deux instances emu), comparer les scores
2. **Replay** : enregistrer la séquence de boutons et la rejouer (seed fixe)
3. **Visualization** : afficher le scene snapshot sous forme de grille ASCII en temps réel
4. **Méta-jeu** : le LLM cherche activement la 3D (appuie sur des boutons jusqu'à ce que
   `get_gte_trace_summary` retourne des PCs) — combine la démo fun ET l'analyse sérieuse
