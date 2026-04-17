# Faisabilité LLM local pour la classification 3D R3000-Emu

**Version**: 1.0 — 2026-04-17
**Contexte**: mission "classification automatique des boucles de rendu 3D PS1" pilotée par LLM via MCP (emu + Ghidra).
**Question**: peut-on faire tourner ce workflow avec un LLM local au lieu de Claude/GPT frontier ?
**Réponse courte**: **oui, c'est jouable dès maintenant.** Voir §1.

> ⚠ **Fraîcheur des chiffres benchmarks** : les numéros Qwen 3.6 Plus / Gemma 4 de §2 viennent d'une passe WebSearch du 2026-04-17. Ils sont **non-validés** indépendamment — les sources en §Sources sont des blogs et wiki communautaires qui peuvent sur-vendre. Avant toute décision d'achat hardware ou de migration cloud→local, **re-vérifier** les benchmarks critiques directement sur :
> - `huggingface.co/Qwen/Qwen3-Coder-Next` (source primaire modèle)
> - `deepmind.google/models/gemma/gemma-4/` (source primaire Google)
> - `openrouter.ai/qwen/qwen3.6-plus-preview/performance` (métriques live)
>
> Les tendances (écart frontier/local qui se ferme ; MoE 26B compétitif ; 1M context courant) sont solides et multi-sources. Les pourcentages exacts sont à confirmer.

---

## 1. TL;DR

| Phase | Modèle recommandé | Hardware | Verdict |
|---|---|---|---|
| **Bootstrap** (premiers jeux, catalogue initial) | Qwen 3.6 Plus (cloud gratuit OpenRouter) OU Gemma 4 31B Dense local | — | ✅ jouable |
| **Classification bulk** (jeux suivants, pattern-matching sur catalogue) | Gemma 4 26B MoE Q4 (3.8B actifs) | 1× RTX 4090 24GB | ✅ jouable |
| **Discovery nouveau pattern exotique** | Qwen 3.6 Plus OU Claude 4.7 frontier | — | ✅ jouable |
| **Experiments rapides / sampling analysis** | Gemma 4 E4B (4.5B) | Laptop 8GB RAM | ✅ jouable |

**La donnée clé** : Qwen 3.6 Plus (mars 2026) **bat Claude 4.5 Opus** sur Terminal-Bench 2.0 agentic coding (61.6 vs 59.3). L'écart frontier/local, qui existait encore fin 2025, s'est effondré au Q1 2026. Le cloud reste pratique pour l'UX (caching, session memory, intégration IDE), pas nécessaire pour la qualité de reasoning.

---

## 2. État de l'art LLM au 17 avril 2026

### Qwen 3.6 Plus Preview (Alibaba, mars 2026)

- **Architecture** : hybride (dense + MoE) optimisée pour agentic.
- **Context** : 1M tokens (vs 200k Claude 4.5).
- **Output max** : 65 536 tokens.
- **Benchmarks pertinents pour notre mission** :
  - SWE-bench Verified : **78.8** (classification et fix de code sur repo réel)
  - Terminal-Bench 2.0 agentic : **61.6** (bat Claude 4.5 Opus à 59.3)
  - HumanEval : high 80s à low 90s (= Claude 3.5 Sonnet / GPT-4o)
- **Dispo** : gratuit via OpenRouter en preview.
- **Variante compacte** : Qwen3-Coder-Next — 3B actifs, SWE-bench-Pro au niveau de modèles 10-20× plus gros.

### Gemma 4 (Google DeepMind, 2 avril 2026)

- **Architecture** : Dense + MoE, multimodale (text + image, audio sur small).
- **4 tailles** :
  | Variante | Taille effective | VRAM Q4 | Context | Cas d'usage |
  |---|---|---|---|---|
  | **E2B** | 2.3B effectifs | 4 GB | 128k | Smartphone |
  | **E4B** | 4.5B effectifs | 5 GB | 128k | Laptop 8GB |
  | **26B A4B MoE** | 3.8B actifs / 26B total | ~12 GB | 128k | 4090 24GB |
  | **31B Dense** | 31B | ~19 GB (Q4), 80GB (full) | 256k | Station puissante / 80GB H100 |
- **Benchmarks 31B Dense** :
  - AIME 2026 : **89.2%** (raisonnement mathématique)
  - LiveCodeBench : **80%**
  - tau2-bench agentic : **76.9%**
- **Benchmarks 26B MoE (3.8B actifs)** :
  - AIME 2026 : **88.3%** (quasi identique au 31B dense avec ~8× moins d'inférence)
- **License** : Apache 2.0 (distribution libre).

### Comparaison pour *notre* mission spécifique

La classification de render loops PSX demande :

| Capacité | Qwen 3.6+ | Gemma 4 31B | Gemma 4 26B MoE | Claude 4.5+ |
|---|---|---|---|---|
| Comprendre MIPS assembleur | 🟢 très bon | 🟢 bon | 🟢 bon | 🟢 très bon |
| Comprendre C décompilé Ghidra | 🟢 très bon | 🟢 bon | 🟡 OK | 🟢 très bon |
| Pattern matching SDK PSYQ (RCpolyGT4, GsPrstXX, etc.) | 🟢 très bon | 🟢 bon | 🟢 bon | 🟢 très bon |
| Multi-step tool calling MCP | 🟢 très bon | 🟢 bon | 🟡 OK avec playbook | 🟢 très bon |
| Long context (dumps Ghidra 50k+ tokens) | 🟢 1M | 🟢 256k | 🟡 128k | 🟢 200k |
| Discovery nouveau pattern exotique | 🟢 | 🟡 | 🟡 | 🟢 |
| Cost par classification | 💰 (gratuit OR) | 💰💰 (élec.) | 💰 (élec.) | 💰💰💰 (API) |

---

## 3. Architecture pipeline MCP + LLM local

```
┌──────────────────────────┐    ┌──────────────────────────┐
│   r3000_emu.exe          │    │        Ghidra            │
│   (Ridge Racer running)  │    │  (PSX-EXE loaded)        │
│   TCP 9742               │    │  MCP plugin port 8080    │
└──────────┬───────────────┘    └──────────┬───────────────┘
           │                                │
           │ JSON                           │ HTTP
           ▼                                ▼
    ┌─────────────────────────────────────────────┐
    │  r3000_mcp.exe (bridge stdio ↔ TCP 9742)    │
    │  bridge_mcp_ghidra.py (stdio ↔ HTTP 8080)   │
    └─────────────────┬───────────────────────────┘
                      │ MCP stdio
                      ▼
     ┌─────────────────────────────────────┐
     │  LLM client (avec function calling) │
     │  ─ Claude Code (actuellement)       │
     │  ─ ou LM Studio + Gemma 4           │
     │  ─ ou ollama + Qwen 3.6-quant       │
     │  ─ ou Open WebUI + Aider + OpenRouter│
     └─────────────────────────────────────┘
```

**Point crucial** : les MCPs emu + Ghidra **parlent le protocole standard MCP**. N'importe quel LLM client qui supporte function calling peut les consommer — pas de lock-in Claude.

### Clients LLM locaux qui marchent avec MCPs

| Client | Support MCP | Modèles | Notes |
|---|---|---|---|
| **LM Studio** | ✅ natif depuis v0.3.x (2026) | Tous GGUF | UI grand public, OpenAI-compatible API |
| **Ollama** + **Continue.dev** | ✅ via adapter | Tous ollama | Scriptable, CLI-friendly |
| **Aider** | ✅ | OpenRouter, local via LiteLLM | Excellent pour code, agentic |
| **Open WebUI** | ✅ | OpenAI-compat backends | UI web, multi-session |
| **Cursor / Windsurf / Cline** | ✅ | Tous | IDE-integrated, equivalents Claude Code |

---

## 4. Config hardware recommandée (ordre de préférence)

### Config 1 — **All-local, une seule machine** (recommandé)

- **GPU** : 1× RTX 4090 24GB (ou 6000 Ada 48GB, ou Mac Studio M3 Ultra 64GB)
- **RAM système** : 64 GB (charger Ghidra Java + emu + modèle)
- **Disque** : 200 GB SSD pour les modèles locaux
- **Modèle principal** : Gemma 4 26B MoE Q4 (~12 GB VRAM, 3.8B actifs → ~40 tokens/s sur 4090)
- **Modèle secondaire** : Qwen3-Coder-Next 3B pour PC sampling analysis rapide

Coût one-shot : ~2500€ (4090 + RAM), 0€ récurrent.

### Config 2 — **Hybrid cloud/local**

- **Laptop** avec 16-32 GB RAM → Gemma 4 E4B pour PC sampling, scripts
- **Cloud** Qwen 3.6 Plus via OpenRouter pour les passes lourdes (gratuit en preview)
- **Fallback** Claude 4.5/4.7 API ($3/M input) pour architectural decisions

Coût récurrent : presque 0€ (Qwen free tier), quelques € par mois de Claude si débordement.

### Config 3 — **Full cloud** (statut quo Claude Code)

- **Claude Code** comme pilote — ce qu'on fait actuellement
- Coût récurrent : abonnement Claude Pro/Team

Inconvénient : dépendance plan Anthropic, session perdue à chaque reboot, rate limits.

---

## 5. Plan de validation A/B

Pour trancher sans débat "sur la théorie" : **test contrôlé** sur Ridge Racer (notre jeu pilote déjà chargé).

### Protocole

1. **Capturer un dump complet** : 500 samples PC + dumps mémoire à des hotspots + décompile Ghidra d'une fonction SDK labellée (ex: `RCpolyGT4 @ 0x80149ac4`).
2. **Même prompt** donné à 3 modèles en parallèle :
   - Claude 4.7 (frontier actuel)
   - Qwen 3.6 Plus (cloud gratuit)
   - Gemma 4 26B MoE local Q4
3. **Prompt** : "Voici un dump PC + MIPS asm + C décompilé. Classifie la boucle de rendu dans le catalogue A-F, explique ton raisonnement, propose un `ModeRule` draft pour `.psx3dprof`."
4. **Métriques** :
   - Justesse de la classification (validable humainement sur Ridge Racer = Type A)
   - Qualité de la `ModeRule` draft (syntaxe correcte ? ranges PC cohérents ?)
   - Temps d'inférence
   - Tokens consommés (coût proxy)

### Résultat attendu (hypothèse)

Qwen 3.6 Plus ≥ Claude 4.7, Gemma 4 26B MoE ~ 90% de Claude 4.7. Si confirmé → **bascule local justifiable immédiatement**.

---

## 6. Points de vigilance

### 6.1 Tool-calling fiabilité

Les LLMs locaux, même 30B+, font **plus d'erreurs de tool args** que frontier (param manquant, mauvais type JSON, fausse adresse hex). Mitigations :

- **Schéma strict avec validation Pydantic** côté MCP bridge
- **Retry with correction prompt** : si l'appel foire, renvoyer l'erreur au LLM et demander fix
- **Playbook structuré** : donner des exemples concrets de tool calls réussis en few-shot

### 6.2 Context management

Qwen 3.6 Plus 1M context est exceptionnel, mais Gemma 4 à 128k peut être saturé par un gros dump Ghidra. Mitigations :

- **Segmenter Ghidra en function-level** : ne jamais envoyer tout le programme, juste les fonctions proches du hotspot
- **Pre-summarize** : script qui résume `disassemble_function(X)` en pseudo-code C avant de l'envoyer au LLM
- **RAG sur le catalogue** : au lieu de réexpliquer A-F à chaque requête, les stocker comme un vector DB de patterns

### 6.3 Déterminisme pour la régression

Les LLMs ne sont pas déterministes par défaut. Pour un pipeline de CI qui valide "Tekken est toujours Type C", il faut :

- **Température 0** dans les appels (ou seed fixé)
- **Sauver les réponses brutes** pour diff
- **Valider par métriques objectives** (la `ModeRule` produite parse-t-elle ? Les PC ranges sont-ils cohérents ?), pas par "le texte est identique"

### 6.4 Apple Silicon (pour Mac Studio config)

Gemma 4 et Qwen 3.6 tournent bien via **llama.cpp Metal** ou **MLX**. MLX est souvent plus rapide sur Apple Silicon mais a parfois un léger lag sur les nouveaux modèles (supporte en 1-4 semaines après release). Vérifier la compat au moment de setup.

### 6.5 Quantization quality loss

Q4 (4-bit) sur Gemma 4 26B MoE = presque indiscernable de Q8. Mais si tu as 24GB VRAM et besoin de **max quality**, Q5_K_M sur 26B MoE tient encore en mémoire et donne un gain marginal. Éviter Q3/Q2 — trop de dégradation sur agentic.

---

## 7. Migration pragmatique depuis Claude Code

**Étape 1** — `mcp__r3000__*` + `mcp__ghidra__*` déjà prêts dans `.mcp.json`. Un client LLM local peut les consommer immédiatement avec **exactement la même surface** que Claude Code.

**Étape 2** — Installer **LM Studio** (UI grand public) ou **Ollama + Aider** (CLI power user) :

```bash
# Ollama + Gemma 4 26B MoE (via placeholder — à ajuster selon release officielle Ollama)
ollama pull gemma4:26b-moe-q4_k_m
ollama serve

# Aider pointé sur Ollama local + nos MCPs
aider --model ollama/gemma4:26b-moe-q4_k_m --openai-api-base http://localhost:11434/v1
```

**Étape 3** — Configurer le MCP bridge dans Aider / LM Studio pour qu'il spawn `r3000_mcp.exe` et `bridge_mcp_ghidra.py` au startup. Format `mcp.json` similaire au fichier projet actuel.

**Étape 4** — Tourner le **même prompt** de classification qu'avec Claude Code, comparer le résultat. Si OK → bascule définitive.

**Étape 5** (optionnel) — Écrire le **playbook** dans `docs/PSX_RENDER_LOOP_PLAYBOOK.md` avec les patterns A-F, leurs signatures MIPS, leurs `ModeRule` templates. Ça transforme la tâche d'ouverte → fermée → accessible aux LLMs plus petits.

---

## 8. Ce qu'on garde dans le cloud quoi qu'il arrive

Même en full-local, certaines tâches restent justifiables en cloud :

- **Architectural planning** (comme ce doc) — besoin de raisonnement long, multi-source, peu fréquent → coût marginal Claude acceptable
- **Recherche web pour specs récentes** — WebSearch n'existe pas en local par défaut
- **Code review final** avant merge — binôme qualité, cross-check sur le LLM principal

---

## 9. Décision recommandée

**Pour le stade actuel de la mission (premiers jeux à cataloguer)** :

1. **Continuer Claude Code** pour ce tour + 1-2 sessions — le setup marche, inutile de casser le momentum
2. **En parallèle**, installer LM Studio + Gemma 4 26B MoE Q4 sur la machine dev (~1h de setup)
3. **Faire le test A/B §5** sur Ridge Racer quand on atteint la première classification complète
4. **Si résultat local ≥ 80% Claude** → bascule local pour les jeux suivants, garder Claude en fallback
5. **Écrire le playbook §7-étape-5** au fur et à mesure — il devient le ciment qui rend le local aussi bon que le cloud

---

## Sources

- Qwen 3.6 Plus review : <https://www.mindstudio.ai/blog/qwen-3-6-plus-review-agentic-coding-model>
- Qwen3-Coder-Next : <https://huggingface.co/Qwen/Qwen3-Coder-Next>
- Qwen 3.6 Plus benchmarks : <https://www.buildfastwithai.com/blogs/qwen-3-6-plus-preview-review>
- Qwen OpenRouter : <https://openrouter.ai/qwen/qwen3.6-plus-preview>
- Gemma 4 DeepMind : <https://deepmind.google/models/gemma/gemma-4/>
- Gemma 4 HF blog : <https://huggingface.co/blog/gemma4>
- Gemma 4 27B local perf guide : <https://www.gemma4.wiki/models/gemma-4-27b>
- Gemma 4 Google Cloud : <https://cloud.google.com/blog/products/ai-machine-learning/gemma-4-available-on-google-cloud>
- Gemma 4 Unsloth docs (local) : <https://unsloth.ai/docs/models/gemma-4>

---

## Historique

- **2026-04-17** v1.0 : création initiale, recherche web sur Qwen 3.6 Plus + Gemma 4, recommandation migration hybrid/local.
