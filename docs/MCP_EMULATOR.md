# MCP Server — R3000-Emu

Le serveur MCP intégré à l'émulateur expose des outils de debug via le protocole MCP (Model Context Protocol). Il permet à Claude Code (ou tout client MCP) d'inspecter et contrôler l'émulateur en temps réel.

## Architecture

```
┌────────────┐     stdio/JSON-RPC      ┌───────────────┐
│ Claude Code │ ◄─────────────────────► │  r3000_mcp.exe│ (CLI standalone)
└────────────┘                          │  ou intégré   │
                                        │  dans UE5     │
                                        └───────┬───────┘
                                                │ IMcpBackend
                                        ┌───────┴───────┐
                                        │   Bus / CPU    │
                                        │   CDROM / GPU  │
                                        └────────────────┘
```

- **Transport** : stdio (JSON-RPC 2.0 avec Content-Length headers)
- **Backend** : interface `IMcpBackend` implémentée par le core émulateur
- **Fichiers** : `src/emu/mcp_server.h`, `src/emu/mcp_server.cpp`

### Note Windows

En mode `--mcp-stdio`, `stdin/stdout` sont forcés en **binaire** dans le CLI.
Sinon Windows traduit `\n` en `\r\n`, ce qui corrompt les headers MCP (`\r\n\r\n` devient `\r\r\n\r\r\n`).

## Configuration

### Claude Code (`.claude/mcp.json`)
```json
{
  "mcpServers": {
    "r3000-debug": {
      "command": "E:\\Projects\\github\\Live\\R3000-Emu\\lib\\Release\\r3000_mcp.exe",
      "args": ["--port", "9742"]
    }
  }
}
```

### UE5
Le MCP server est intégré dans le plugin UE5 R3000Emu. Il se lance automatiquement quand le component R3000Emu est actif.

## Outils disponibles

### Diagnostic

| Outil | Description | Paramètres |
|-------|-------------|------------|
| `emu.ping` | Vérifie que le serveur est vivant | — |
| `emu.get_status` | Status émulateur : has_core, has_cpu, frame_count, tri_3d/2d, profile | — |
| `emu.get_cpu_state` | Snapshot CPU : PC, HI, LO, GPR[32] | — |

### Mémoire

| Outil | Description | Paramètres |
|-------|-------------|------------|
| `emu.read_ram_u32` | Lit un uint32 en RAM physique | `phys_addr` (int) |
| `emu.read_cop0` | Lit un registre COP0 | `reg` (0-31) |
| `emu.write_cop0` | Écrit un registre COP0 | `reg` (0-31), `value` |
| `emu.add_step_hook_write_cop0` | Ajoute un hook de step qui écrit COP0 à un PC précis | `pc`, `reg`, `value`, `once` (opt) |
| `emu.add_step_hook_write_ram_u32` | Ajoute un hook de step qui écrit en RAM à un PC précis | `pc`, `phys_addr`, `value`, `once` (opt) |
| `emu.list_step_hooks` | Liste les step hooks MCP actifs | — |
| `emu.clear_step_hook` | Retire un step hook par id | `hook_id` |
| `emu.clear_all_step_hooks` | Retire tous les step hooks MCP | — |
| `emu.add_mem_watch_write` | Ajoute un watchpoint léger sur écriture RAM, avec filtres optionnels PC/valeur | `phys_addr_start`, `phys_addr_end` (opt), `pc` (opt), `value` (opt), `once` (opt) |
| `emu.list_mem_watches` | Liste les watchpoints RAM actifs | — |
| `emu.clear_mem_watch` | Retire un watchpoint RAM par id | `watch_id` |
| `emu.clear_all_mem_watches` | Retire tous les watchpoints RAM | — |
| `emu.list_mem_watch_events` | Liste les hits récents capturés dans le ring buffer | — |
| `emu.clear_mem_watch_events` | Vide le ring buffer d'événements | — |
| `emu.run_until_mem_watch` | Exécute jusqu'au prochain hit de watchpoint mémoire ou épuisement du budget | `max_steps` (1-100M) |
| `emu.list_logs` | Lit les entrées récentes de `emu::log` capturées par le sink async | `since_seq` (opt), `min_level` (opt), `tag` (opt), `contains` (opt), `max_entries` (opt) |
| `emu.clear_logs` | Vide le ring buffer des logs capturés | — |

**Note** : L'adresse est **physique** (pas virtuelle). Pour convertir : `phys = virt & 0x1FFFFF`.

Exemples :
```
emu.read_ram_u32  phys_addr=0x000D19B4   → Soul Reaver vi (virt 0x800D19B4)
emu.read_ram_u32  phys_addr=0x000CD2E0   → Soul Reaver cd2e0 callback
emu.read_ram_u32  phys_addr=0x00034520   → Crash tick counter (virt 0x80034520)
emu.read_cop0     reg=9                  → BDAM sur R3000A
emu.write_cop0    reg=9 value=0          → force BDAM à 0 pour test
emu.add_step_hook_write_cop0 pc=0x800C12F8 reg=9 value=0 once=true
emu.add_step_hook_write_ram_u32 pc=0x800C12F8 phys_addr=0x000CEEC0 value=1 once=true
emu.add_mem_watch_write phys_addr_start=0x000CD2E4 phys_addr_end=0x000CD2E7
emu.add_mem_watch_write phys_addr_start=0x000CD2E4 pc=0x800C13B4 once=true
emu.run_until_mem_watch max_steps=10000000
emu.list_mem_watch_events
emu.list_logs tag=\"CD\" min_level=3 max_entries=64
```

**Sémantique des watchpoints RAM**
- Le watchpoint surveille les écritures **RAM physique** seulement.
- Le match adresse est fait par **intersection de plage** avec la taille réelle de l’écriture (1/2/4 octets).
- Le filtre `pc` est optionnel et compare le `PC` courant au moment de l’écriture.
- Le filtre `value` est optionnel et compare la valeur brute écrite (taille 1/2/4 selon le write).
- Les hits sont stockés dans un petit **ring buffer** MCP pour éviter de spammer les logs.
- Coût quand inutilisé :
  le hook write reste branché dans le backend MCP, mais retourne immédiatement si aucun watchpoint n’est actif.

**Sémantique des logs MCP**
- Les entrées viennent de `emu::log`, donc du même pipeline async que les logs fichier/`stderr`.
- Le thread de log continue d’écrire sur `stderr`, et recopie aussi les entrées dans un petit ring buffer mémoire.
- `since_seq` permet une lecture incrémentale sans rescanner tout l’historique.
- Les filtres `tag`, `min_level` et `contains` servent à recoller vite un watchpoint mémoire avec les sous-systèmes concernés.
- `json_escape()` du serveur MCP échappe maintenant aussi tous les caractères de contrôle `< 0x20`, ce qui évite les réponses JSON invalides quand un log contient des caractères spéciaux.

### Exécution

| Outil | Description | Paramètres |
|-------|-------------|------------|
| `emu.step` | Exécute N instructions CPU | `count` (1-100000) |
| `emu.run_until_breakpoint` | Exécute jusqu'à breakpoint ou budget | `max_steps` (1-100M) |

### Breakpoints

| Outil | Description | Paramètres |
|-------|-------------|------------|
| `emu.set_breakpoint_pc` | Ajoute un breakpoint PC | `pc` (int) |
| `emu.clear_breakpoint_pc` | Retire un breakpoint PC | `pc` (int) |
| `emu.clear_all_breakpoints` | Retire tous les breakpoints | — |
| `emu.list_breakpoints` | Liste les breakpoints actifs | — |

### PSX3D (reconstruction 3D)

| Outil | Description | Paramètres |
|-------|-------------|------------|
| `emu.set_psx3d_mode` | Mode "game" ou "analysis" | `mode` ("game"\|"analysis") |
| `emu.request_psx3d_refresh` | Demande un refresh de l'analyse 3D | `reason` (string), `scope` (string, opt) |
| `emu.set_gte_trace_window` | Configure la capture GTE par range PC et frames | `pc_start`, `pc_end`, `start_frame`, `end_frame`, `enabled` |

## Exemples d'utilisation

### Lire l'état du jeu
```
→ emu.get_cpu_state
← pc=0x800390CC, gpr[2]=5, ...

→ emu.read_ram_u32  phys_addr=0x000D19B4
← value=5  (Soul Reaver vi)
```

### Poser un breakpoint et exécuter
```
→ emu.set_breakpoint_pc  pc=0x800390CC     (quand vi est écrit)
← breakpoint set

→ emu.run_until_breakpoint  max_steps=10000000
← breakpoint hit, hit_pc=0x800390CC, steps_done=4523891

→ emu.get_cpu_state
← pc=0x800390CC, gpr[...]
```

### Debug workflow typique
```
1. emu.ping                          → vérifier la connexion
2. emu.get_status                    → vérifier que le core tourne
3. emu.set_breakpoint_pc  pc=0x...   → poser un breakpoint
4. emu.run_until_breakpoint          → exécuter jusqu'au breakpoint
5. emu.get_cpu_state                 → lire les registres
6. emu.read_ram_u32                  → lire la RAM
7. emu.step  count=10                → avancer instruction par instruction
8. emu.clear_all_breakpoints         → nettoyer
```

## Deuxième MCP : DuckStation GDB

En complément, un MCP Python (`scripts/mcp_duckstation.py`) permet de se connecter au serveur GDB de DuckStation pour **comparer** l'état des deux émulateurs au même point d'exécution.

Voir `docs/SESSION_2026-04-07-08.md` pour les détails.

## Limitations actuelles

- **Pas de write RAM direct générique** : seulement via step hooks (pas encore d’outil `write_ram_u32` simple)
- **Pas de watchpoints lecture** : cette première version couvre les écritures RAM
- **Pas de read MMIO** : lecture RAM physique uniquement, pas des registres I/O
- **Single-shot** : pas de mode "stream" pour observer en continu
- **Adresses physiques** : le client doit convertir les adresses virtuelles lui-même

## Extension possible

Pour le debug Soul Reaver / LibCrypt, il serait utile d'ajouter :
- `emu.read_mmio` — lire les registres I/O (I_STAT, I_MASK, CDROM regs, timers)
- `emu.write_ram_u32` — écrire en RAM (forcer cd2e0=0 pour bypass LibCrypt)
- `emu.add_watchpoint` — breakpoint sur écriture mémoire (comme le vi watchpoint)
- `emu.get_cdrom_state` — état CDROM (irq_flags, reading_active, read_lba, etc.)
- `emu.get_timer_state` — état des 3 timers (count, mode, target, enabled)
