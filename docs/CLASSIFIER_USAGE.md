# Classifier Usage

Le classifier Python vit dans [scripts/classifier](/E:/Projects/github/Live/R3000-Emu/scripts/classifier).

## Venv

Créer l'environnement dédié:

```bat
E:\Projects\github\Live\R3000-Emu\scripts\classifier\create_venv.bat
```

Lancer ensuite le classifier via le wrapper:

```bat
E:\Projects\github\Live\R3000-Emu\scripts\classifier\run_classifier.bat --help
```

Le `venv` est local à `scripts/classifier/.venv`, pour éviter de polluer l'environnement Python utilisateur.

## Prérequis

- Build `Release` de l'émulateur présent
- `r3000_emu.exe` accessible via `scripts/classifier/config.yaml`
- Ghidra ouvert avec le plugin HTTP/MCP actif sur `http://localhost:8080` si on veut la partie statique complète
- LM Studio, Ollama ou autre backend LLM compatible si la phase LLM est activée

Le classifier moderne utilise une seule session native `r3000_emu.exe --mcp-stdio` pour tout le run:

1. boot contrôlé
2. analyse statique
3. reprise runtime
4. classification
5. génération rapport/profile

`r3000_mcp.exe` reste un bridge legacy utile pour d'autres workflows, mais ce n'est plus le backend principal du classifier.

## Sens de `--launch-mode`

- `paused` ou `pause_after_exe_load`:
  l'émulateur avance jusqu'au moment où le code jeu est chargé et prêt
  (sortie de la zone BIOS / entrée dans le runtime EXE ou menu), puis le
  classifier s'arrête pour commencer l'analyse statique depuis ce contexte-là.
- `pause_immediate`:
  arrêt immédiat au tout début de la session, utile pour du debug BIOS très bas niveau.
- `run_to_ram`:
  l'émulateur avance jusqu'au runtime puis l'orchestrateur considère cette phase déjà reprise.

## Option `--track-runtime-modules`

Le classifier peut activer un suivi grossier des transitions de régions de code
runtime via:

```bat
--track-runtime-modules
```

Ce suivi reste **désactivé par défaut** pour éviter de l'imposer à tous les runs.
Quand il est activé, l'émulateur expose aussi `emu.get_runtime_module_history`
pour aider l'orchestrateur ou une analyse manuelle à repérer les bascules
boot EXE -> menu -> runtime -> autre module.

## LM Studio

Le classifier sait parler à LM Studio via l'API OpenAI-compatible.

Exemple recommandé:

```bat
E:\Projects\github\Live\R3000-Emu\scripts\classifier\run_classifier.bat ^
  --game TREX ^
  --exe "E:\Projects\PSX\roms\TREX.EXE" ^
  --devkit-hle ^
  --launch-mode paused ^
  --llm-provider lmstudio ^
  --llm-model gemma-4-31b-it-uncensored-heretic ^
  --llm-api-base http://127.0.0.1:1234/v1
```

Note:
- pour LM Studio, le client injecte une clé factice locale si aucune vraie clé n'est fournie
- le bon modèle exposé par LM Studio doit être visible sur `/v1/models`

## Narration / TTS

La narration du classifier passe par le hook projet:

- [scripts/classifier/narrator.py](/E:/Projects/github/Live/R3000-Emu/scripts/classifier/narrator.py)
- [.claude/hooks/tts.sh](/E:/Projects/github/Live/R3000-Emu/.claude/hooks/tts.sh)
- [.claude/hooks/play-tts.sh](/E:/Projects/github/Live/R3000-Emu/.claude/hooks/play-tts.sh)
- [.claude/hooks/play-tts-piper.sh](/E:/Projects/github/Live/R3000-Emu/.claude/hooks/play-tts-piper.sh)

Chemin retenu:

1. le classifier lance `Narrator.speak(...)`
2. `Narrator` appelle explicitement le hook projet `.claude/hooks/tts.sh`
3. le hook choisit la voix FR/EN
4. `play-tts-piper.sh` synthétise l'audio avec Piper
5. le WAV est joué puis conservé dans [E:\Projects\github\Live\R3000-Emu\.claude\audio](/E:/Projects/github/Live/R3000-Emu/.claude/audio)

Pré-requis actuels pour cette machine:

- `bash` disponible via WSL
- voix Piper présentes dans `C:\Users\surfu\.claude\piper-voices`
- provider actif `piper`
- hooks `.sh` en fins de ligne LF

Point important d'architecture:

- le classifier utilise maintenant un chemin explicite vers le hook projet
- côté Piper, le code détecte explicitement le cas WSL + wrapper Windows `piper.exe`
- dans ce cas, il appelle directement le vrai `piper.exe` Windows avec des chemins Windows
- s'il manque le hook ou le backend Piper attendu, on veut une erreur claire plutôt qu'un fallback silencieux

La config TTS par défaut reste dans [scripts/classifier/config.yaml](/E:/Projects/github/Live/R3000-Emu/scripts/classifier/config.yaml):

- `tts.enabled: true`
- `tts.voice: nova`
- `tts.language: auto`

Exemple rapide de test:

```bat
E:\Projects\github\Live\R3000-Emu\scripts\classifier\.venv\Scripts\python.exe -c "from scripts.classifier.narrator import Narrator; from scripts.classifier.config import TtsConfig; Narrator(TtsConfig(enabled=True, command_template='', voice='nova', language='auto')).speak('test', 'Salut narration propre')"
```

## Politique LLM

Le classifier n'a plus de fallback heuristique local pour la phase de
classification. Un backend LLM réel est donc requis pour un run complet.

Pour `OpenRouter`, vérifier que `OPENROUTER_API_KEY` est défini avant le run.
Pour `LM Studio` ou `Ollama` en mode OpenAI-compatible, fournir `--llm-api-base`.

## Philosophie de run

Le flux par défaut est:

1. lancer l'émulateur et avancer jusqu'au runtime EXE/menu
2. faire une découverte statique d'abord
3. reprendre l'émulateur pour la corrélation runtime
4. demander une classification au LLM
5. écrire:
   - un JSONL de session dans `logs/classifier/`
   - un rapport markdown dans `logs/classifier_reports/`
   - un profile généré dans `psx3dprof/`

## Stratégie actuelle de l'orchestrateur

Le classifier n'est plus pensé comme un run "one shot".

La stratégie actuelle est:

1. `boot aware`
   - lancer l'emu
   - attendre que le boot EXE soit reconnu et prêt
   - s'arrêter à ce moment-là pour commencer depuis un contexte utile

2. `runtime first`
   - utiliser d'abord l'emu MCP pour repérer les vrais signaux:
     - activité GTE
     - producteurs OT / DMA2 / draw list
     - groupes visibles
     - roots de transforms
   - le but est de trouver quoi regarder, pas encore tout comprendre

3. `ghidra deep pass`
   - utiliser ensuite Ghidra sur les PCs et fonctions candidates
   - inventorier les opcodes GTE vus dans le code
   - explorer les branches, xrefs, callers/callees et wrappers Sony 3D probables
   - commencer à répondre à des questions comme:
     - lien direct ou non entre sortie GTE et remplissage polygons
     - vertex fill vs poly fill
     - builder intermédiaire vs OT direct

Réglages associés dans [scripts/classifier/config.yaml](/E:/Projects/github/Live/R3000-Emu/scripts/classifier/config.yaml):

- `workflow.ghidra_branch_depth`
- `workflow.ghidra_branch_fanout`
- `workflow.ghidra_static_seed_limit`

Ils contrôlent respectivement:

- jusqu'où l'orchestrateur remonte/descend dans les branches Ghidra
- combien de voisins par noeud il suit
- combien de graines statiques il prend au départ

4. `multi-pass`
   - si le signal runtime est faible, l'orchestrateur ne s'arrête pas au premier essai
   - il peut faire plusieurs passes:
     - reprise runtime
     - nouvelles observations
     - probes pad
     - nouvelle tentative de classification

5. `mémoire externe, pas gros contexte`
   - le classifier n'essaie pas de tout garder dans le prompt
   - il écrit des artefacts persistants:
     - JSONL
     - rapport
     - profile
     - snapshots mémoire structurés
   - les phases suivantes relisent et résument ces preuves au lieu d'empiler du contexte brut

6. `classification LLM réelle`
   - il n'y a plus de fallback heuristique local pour la classification finale
   - si un LLM est activé, on veut une vraie décision LLM à partir des preuves
   - si les preuves sont insuffisantes, la sortie doit rester prudente et l'assumer

En pratique, la logique visée est:

- l'emu MCP sert d'abord à localiser
- Ghidra sert ensuite à comprendre
- l'emu MCP revalide si besoin
- le rapport final synthétise les preuves et génère la base du `.psx3dprof`

## Artefacts mémoire

Pour limiter la dépendance au contexte LLM, le classifier écrit maintenant une
mémoire externe structurée dans:

- [logs/classifier_memory](/E:/Projects/github/Live/R3000-Emu/logs/classifier_memory)

Cette mémoire sert à:

- conserver les notes importantes entre phases
- reconstruire un contexte compact pour le LLM
- éviter de "réinventer" l'analyse à chaque nouvelle passe

Les snapshots de mémoire complètent:

- les JSONL de session
- les rapports markdown
- les profiles générés

## Sorties

- JSONL: `E:\Projects\github\Live\R3000-Emu\logs\classifier\`
- rapports: `E:\Projects\github\Live\R3000-Emu\logs\classifier_reports\`
- profiles générés: `E:\Projects\github\Live\R3000-Emu\psx3dprof\`
