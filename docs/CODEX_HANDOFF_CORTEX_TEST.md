# Handoff Claude - Branch `cortex-test`

Date: 2026-03-05

## Objectif en cours

Prototype "dataflow token" (sans fallback) pour reconstruire le lien GTE->GPU3D sans
injecter de coordonnees taggees dans le pipeline principal.

Flux cible:
- COP2 (MFC2/SWC2) -> token face
- token propage vers RAM
- DMA2 lit RAM + token
- Gpu3D utilise le token pour `face_idx`

## Etat actuel (progressif)

### Branche
- Branche locale creee: `cortex-test`

### Mise a jour recente (generic auto-adapt)

20. `Psx3dModeManager` + modes runtime
- Fichiers:
  - `src/emu/psx3d_mode_manager.h`
  - `src/emu/psx3d_mode_manager.cpp`
- Integration `Core`:
  - mode `game|analysis`
  - autorisation d'analyse ON/OFF
  - file de requetes refresh (`reason/scope`)
- CLI:
  - `--psx3d-mode=game|analysis`
  - `--psx3d-analysis=0|1`
  - `--psx3d-refresh=reason:scope`

21. Stats miss decode GPU3D clarifiees
- `src/gpu/gpu_3d.cpp/.h`:
  - compteurs ajoutes:
    - `miss_no_hint`
    - `miss_hint_stale`
    - `miss_decode_fail`
- Diagnostic: sur les logs user, les trous viennent majoritairement de `miss_no_hint`
  (pas d'indice token en amont), pas d'un cache stale.

22. Tracage causal DMA2 nohint par PC ecrivain RAM
- `src/r3000/bus.h/.cpp`:
  - stockage writer PC par mot RAM (`ram_face_writer_pc_`)
  - au DMA2 GPU: si token absent, histogramme par writer PC
  - resume VBlank consommable via API:
    - `consume_dma2_nohint_summary(...)`
    - struct `Dma2NoHintSummary { vblank, nohint_words, top_pcs }`
  - logs `DMA2_NOHINT` top PCs

23. Profiler generic des hotspots (auto refresh)
- Fichiers:
  - `src/emu/provenance_hotspot_profiler.h`
  - `src/emu/provenance_hotspot_profiler.cpp`
- Integration `Core::step()`:
  - ingest des `Dma2NoHintSummary`
  - decision generique avec cooldown
  - auto queue d'une requete refresh:
    - reason=`auto_hotspot`
    - scope=`pc=0xXXXXXXXX`

24. Build chain
- `CMakeLists.txt` maj:
  - ajout `src/emu/provenance_hotspot_profiler.cpp` dans `CORE_SOURCES`
- Build verifie:
  - `cmake -S . -B build`
  - `cmake --build build --config Debug -j 8` OK

25. Persistance profil analyse par jeu (evite de refaire l'analyse)
- Nouveaux fichiers:
  - `src/emu/psx3d_profile_store.h`
  - `src/emu/psx3d_profile_store.cpp`
- Profil persiste:
  - hotspots provenance (`pc`, `total_words`, `last_seen_vblank`, `last_refresh_vblank`)
- Format fichier:
  - texte V1 (`PSX3D_PROFILE_V1`) simple et diffable
- Emplacement:
  - `profiles/psx3d/<game>.psx3dprof`
- Identification jeu:
  - derivee du nom de fichier du `--cd` (via `insert_disc`) ou du `--load` EXE (via `fast_boot_from_exe`)
- Integration Core:
  - autoload du profil quand identity connue
  - dirty flag quand nouvelles stats analyse ingerees
  - autosave periodique (toutes 600 VBlank si dirty)
  - save final en destructor `Core` (fin de session)

26. Override explicite du profil (UE component)
- Core:
  - API ajoutee: `set_psx3d_profile_path_override(const char* path)`
  - lock override: l'identity auto (`insert_disc`/`fast_boot_from_exe`) n'ecrase plus ce path
- UE5:
  - `UR3000EmuComponent` expose:
    - `UPROPERTY FString Psx3dProfilePath` (Category `R3000Emu|PSX3D`)
  - `InitEmulator()` applique l'override au `Core` avant boot/init.

27. Cycle auto "analyse -> save -> game" (sans rester bloque en analyse)
- `src/emu/core.cpp/.h`
  - ajout set `psx3d_analyzed_pcs_` persiste dans le profil
  - refresh analyse:
    - marque les PCs scopes comme "deja analyses"
    - applique cooldown profiler (`ack_refresh`)
    - `try_save_psx3d_profile()`
    - retour auto `mode=game` apres pass refresh
  - detection runtime:
    - si hotspot DMA2 nouveau PC -> bascule auto en `analysis` + refresh queue
    - si PC deja analyse -> cooldown seulement (pas de spam refresh)
- `src/emu/psx3d_profile_store.h/.cpp`
  - format profil etendu:
    - `ANALYZED_PC 0xXXXXXXXX`
  - chargement/sauvegarde des PCs analyses.

28. Tracker generique "camera root RAM" (premiere passe)
- `src/r3000/cpu.h/.cpp`
  - nouveau tracking quand `analysis_enabled=1`:
    - memorise l'origine memoire recente des registres CPU (loads)
    - observe `MTC2` vers regs GTE `0..11` (RT/TR matrix inputs)
    - corrèle source registre -> adresse RAM candidate
  - sorties logs periodiques:
    - tag `CAM_ROOT`
    - top adresses candidates avec `frame_hits`, `hits`, masque regs GTE touches, `last_pc`
- `src/emu/core.cpp`
  - propagation de `analysis_enabled` vers CPU:
    - `cpu_->set_camera_analysis_enabled(...)`

29. Persistance des candidates camera dans `.psx3dprof`
- `src/emu/psx3d_profile_store.h/.cpp`
  - nouveau bloc profile:
    - `CAM 0xADDR HITS FRAME_HITS FIRST_VBL LAST_VBL 0xLAST_PC 0xREGMASK`
  - load/save trie par `frame_hits` puis `hits`.
- `src/r3000/cpu.h/.cpp`
  - API snapshot/restore:
    - `camera_candidates_snapshot()`
    - `restore_camera_candidates(...)`
    - `camera_candidates_serial()`
- `src/emu/core.cpp`
  - serialize/deserialize camera candidates via `Psx3dProfileData`
  - dirty flag profile declenche seulement si serial camera change.

30. Correctif auto-analyse (logs user 2026-03-06)
- Probleme observe:
  - refresh `high_fallback_2d` faisait `added_pcs=0` car scope non pris en charge.
- Fix:
  - `run_psx3d_analysis_refresh(...)`:
    - pour scopes non `pc=...`/`global`, prend maintenant les top hotspots du profiler
      (budget 32) et les marque analyses + cooldown.
- Bruit camera:
  - top CAM_ROOT etait pollue par `0x1F800xxx` (scratchpad), pas la vraie RAM jeu.
- Fix:
  - tracker camera filtre desormais `addr < ram_size` (RAM principale uniquement)
    en observe + restore profil.

### Note importante
- Le systeme est maintenant structure pour etre **generique** (pas hardcode Ridge Racer):
  - collecte causale par PC runtime
  - detection hotspots dynamique
  - refresh analyse declenche automatiquement
- La phase suivante reste l'analyse plus profonde "call-aware" (trace inter-procedurale)
  pour convertir ces hotspots en regles de provenance stables multi-jeux.

### Modifs deja faites

1. `src/r3000/bus.h`
- Ajout `#include <vector>`
- Ajout constante `kNoFaceToken`
- Ajout API:
  - `set_ram_face_token(uint32_t paddr, uint32_t token)`
  - `ram_face_token(uint32_t paddr) const`
- Ajout stockage:
  - `std::vector<uint32_t> ram_face_tokens_`

2. `src/gpu/gpu_3d.h`
- Ajout constante `kNoFaceHint`
- Ajout API:
  - `gp0_with_face_hint(uint32_t word, uint32_t face_hint)`
- Ajout buffer parallele:
  - `cmd_face_hint_[16]`

3. `src/r3000/cpu.h`
- `PendingLoad` etendu avec `face_token`
- Ajout constante CPU `kNoFaceToken`
- Ajout tableau registre->token:
  - `gpr_face_token_[32]`

4. `src/r3000/bus.cpp`
- Version marker passe a `v26 (face_token_flow)`
- Initialisation `ram_face_tokens_` dans le constructeur
- Implementations ajoutees:
  - `set_ram_face_token(...)`
  - `ram_face_token(...)`
- Clear token sur ecritures RAM (`write_u8/u16/u32`)
- DMA2 GPU:
  - propagation token vers shadow GPU via `gp0_with_face_hint(...)`
  - block mode + linked-list mode couverts
- MMIO direct GPU (`1F801810`) route vers `gp0_with_face_hint(..., kNoFaceToken)`

5. `src/gpu/gpu_3d.cpp`
- `gp0()` devient wrapper de `gp0_with_face_hint(..., kNoFaceHint)`
- buffer parallele `cmd_face_hint_` rempli pendant la collecte GP0
- `gp0_start_command(...)` etendu avec `face_hint`
- `gp0_polygon()`:
  - collecte `face_hints[]` sur les mots XY des vertices
  - derive `face_idx` par majority vote des hints
  - prototype strict: pas de fallback decode differential dans ce chemin

6. `src/r3000/cpu.cpp`
- version marker `v8 (face_token_flow)`
- reset:
  - init `gpr_face_token_[]`
  - init `pending_load_.face_token`
- `set_reg()` clear token destination
- `commit_pending_load()` propage `face_token` vers registre
- `next_pending_load.face_token` initialise dans `step()`
- helpers memoire:
  - `store_u8/u16`: clear token RAM
  - `store_u32`: ecrit token RAM associe
- ajout helper decode token depuis SXY tagge (`decode_face_token_from_tagged_sxy`)
- LW:
  - recupere token RAM et l'attache au pending load
- SW:
  - ecrit token du registre source en RAM
- COP2:
  - MFC2 lit toujours le GTE primaire
  - token derive depuis shadow pour regs SXY (12..15)
  - SWC2 ecrit toujours valeur primaire, token derive shadow

### Build
- Build local OK:
  - `cmake --build build -j 4`
  - sortie: `lib/Debug/r3000_emu.exe`

### Correctif UE5 flicker double-buffer (Y instable)

7. `src/gpu/gpu_3d.cpp`
- Correctif applique sur la conversion des vertices 2D (`make_vertex`):
  - avant: `se11(raw) + draw_env.offset`
  - maintenant: `se11(raw)` uniquement (offset ignore pour le shadow 3D path)
- Raison:
  - `draw_env.offset_y` alterne selon la banque VRAM (double-buffer) et provoque
    un decalage Y frame-a-frame dans la scene UE5 (flicker).
  - Ce composant 3D attend des coords ecran stables, pas des coords VRAM ciblees.
- Build revalide apres patch:
  - `cmake --build build -j 4` OK

8. `src/gpu/gpu_3d.cpp` (trous / polys manquants)
- Selection `face_idx` amelioree dans `gp0_polygon()`:
  - avant: vote majoritaire brut des hints
  - maintenant: vote majoritaire **avec priorite aux tokens presents en face cache**
  - si aucun token hint n'est en cache: on garde le meilleur token hint brut
- Objectif:
  - reduire les primitives qui retombent en 2D a cause d'un token majoritaire stale/non present.
- Contrainte respectee:
  - toujours sans fallback decode differential dans ce chemin.

9. Reduction du bruit logs (warnings)
- `integrations/ue5/.../R3000Gpu3DComponent.cpp`:
  - logs lifecycle/rebuild passes de `Warning` a `Log/Verbose`
  - logs debug detailes `GPU3D` passes de `warn` a `info`
- `src/gpu/gpu_3d.cpp`:
  - logs `GPU3D_DIAG` / `GPU3D_VBLANK` / init passes de `warn` a `info`
- But:
  - garder `Warning` UE5 uniquement pour vrais problemes (ex: materiaux manquants)
  - rendre les vrais warnings lisibles pour diagnostiquer les trous.

10. Fix format texture 2D menu (bit RAW sur polygons)
- Cause probable identifiee:
  - pour GP0 polygons texturés (`20h-3Fh`), le bit `raw texture` (cmd bit0)
    n'etait pas reporte dans `flags`.
  - contrairement a `gp0_rect()`, `gp0_polygon()` n'ajoutait pas `flags |= 4`.
- Impact:
  - le shader peut moduler a tort par la couleur vertex alors que le primitive est raw
    (no modulation), ce qui fausse fortement le rendu (teinte/format percu incorrect).
- Correctif applique:
  - `src/gpu/gpu.cpp` -> `gp0_polygon()` lit `raw` et set `flags |= 4`
  - `src/gpu/gpu_3d.cpp` -> meme fix pour coherence du shadow path
- Build:
  - `cmake --build build -j 4` OK

11. Fix 2D rect path dans `Gpu3D` (cause probable fond menu faux dans composant 3D)
- Contexte:
  - Le user a precise que le probleme est en 2D dans `R3000Gpu3DComponent`.
  - Comparaison `Gpu::gp0_rect()` vs `Gpu3D::gp0_rect()` trouvait des divergences.
- Divergences corrigees dans `src/gpu/gpu_3d.cpp`:
  - ajout support bit `raw texture` (cmd bit0) -> `flags |= 4`
  - UV rect: passage de wrap implicite (`uint8(u0+w)`) a clamp `[0..255]` comme `Gpu`
  - guard sur tailles invalides `w<=0 || h<=0`
- Effet attendu:
  - decode texture/CLUT rects 2D aligne sur le pipeline 2D normal, reduisant les erreurs
    type "16 couleurs lues comme 256" dans le composant 3D.
- Build:
  - `cmake --build build -j 4` OK

11b. Ajustement UV rect `Gpu3D` (drapeau coupe en bas)
- Observation user:
  - apres le fix rect, le drapeau apparaissait "a moitie" (bas manquant).
- Cause probable:
  - clamp UV `[0..255]` sur `gp0_rect()` shadow path coupait certains sprites qui
    comptent sur wrap 8-bit.
- Correctif:
  - rollback vers UV wrap 8-bit naturel (`uint8_t` cast) pour `u1/v1`.
- Fichier:
  - `src/gpu/gpu_3d.cpp`

17. Fix demi-quads manquants (drapeau): token V3 pour `face_B`
- Symptome:
  - "moitie de polygons" manquante sur le drapeau (second triangle de quad).
- Cause probable:
  - `push_quad()` utilisait encore surtout un decode legacy via coords taggees pour trouver `face_B`.
  - en mode token-flow, ce decode n'est souvent plus valide -> fallback degeneré du 2e tri.
- Correctif:
  - ajout d'un hint explicite `face_idx_v3_hint` dans `push_quad(...)`
  - `gp0_polygon()` passe le hint du mot XY de `V3` (`face_hints[3]`)
  - `push_quad()` priorise ce hint pour resoudre `face_B` (edge-strip + fallback non-edge)
- Fichiers:
  - `src/gpu/gpu_3d.h`
  - `src/gpu/gpu_3d.cpp`

18. Instrumentation minimale "trous polygons" (GPU3D_VBLANK)
- Ajout compteurs per-frame (reset a chaque VBlank):
  - `tok_hint`: polygons avec au moins un hint token
  - `tok_miss`: polygons sans hint exploitable
  - `tok_cached`: polygons avec hint resolu en face cache
  - `v3_hint`: quads dont `face_B` a ete resolu via hint explicite V3
- Log ajoute dans `GPU3D_VBLANK`:
  - `... quad_hit=.. quad_miss=.. tok_hint=.. tok_miss=.. tok_cached=.. v3_hint=..`
- Fichiers:
  - `src/gpu/gpu_3d.h`
  - `src/gpu/gpu_3d.cpp`

19. Nouvelle classe CPU: `CpuProvenanceAnalyzer` (cache par PC)
- But:
  - sortir la logique "bidouille" de propagation token hors de `Cpu::step()`
  - analyser une fois par PC puis reutiliser une regle (plus propre/perf)
- Fichiers ajoutes:
  - `src/r3000/cpu_provenance_analyzer.h`
  - `src/r3000/cpu_provenance_analyzer.cpp`
- Integration:
  - membre `provenance_analyzer_` ajoute dans `Cpu`
  - `step()` appelle `apply_cached_provenance()` pour les ops move-like:
    - `SLL sh=0`, `ADDU`, `SUBU`, `OR`, `ADDIU imm=0`, `ORI imm=0`
  - regle deduite et cachee par PC, puis token derive de rs/rt selon regle
- Build:
  - `cmake --build build -j 4` OK

12. Mode camera/mesh dans `R3000Gpu3DComponent`
- Ajout `TrackingMode` (enum):
  - `LegacyFollowOwner` (defaut, comportement actuel)
  - `WorldLocked` (detach mesh en world-space pour free-roam camera)
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

13. Fix trous: propagation token sur acces memoire partiels CPU
- Cause probable:
  - `LWL/LWR/SWL/SWR` ne propageaient pas correctement `face_token`
  - `SB/SH` n'ecrivaient pas le token du registre source
  - `LB/LBU/LH/LHU` ne recuperaient pas de token RAM
- Correctifs (`src/r3000/cpu.cpp`):
  - `store_u8/store_u16` acceptent maintenant un `face_token`
  - `SB/SH` ecrivent `gpr_face_token_[rt]`
  - `SWL/SWR` ecrivent via `store_u32(..., gpr_face_token_[rt])`
  - `LWL/LWR` set `next_pending_load.face_token` depuis `ram_face_token(base)`
  - `LB/LBU/LH/LHU` set `next_pending_load.face_token` depuis `ram_face_token(addr)`
- Build:
  - `cmake --build build -j 4` OK

14. Mode "warnings only" pour `R3000Gpu3DComponent`
- Ajout compile-time define local dans:
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`
- Define:
  - `R3000_GPU3D_WARNINGS_ONLY` (defaut = 1)
- Effet:
  - logs `UE_LOG` niveau `Log/Verbose` de ce composant compiles out
  - logs `emu::logf` niveau info (`GPU3D`) compiles out
  - les `Warning/Error` restent visibles
- Pour reactiver les logs bruyants:
  - passer `R3000_GPU3D_WARNINGS_ONLY` a `0`.

15. Switch transform 3D optionnel dans `R3000Gpu3DComponent`
- Ajout property Blueprint/C++:
  - `bApplyGteTransform` (defaut `true`)
- Comportement:
  - `true`: mode actuel, applique `RT*V + TR` avant mapping UE
  - `false`: utilise directement `verts_3d` bruts (mode exploration/world debug)
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

16. Mode experimental "pseudo world-space" (Gpu3D)
- Ajout property:
  - `bApproxWorldFromFrameRef` (defaut `false`)
- Principe:
  - on reconstruit d'abord en camera-space (`RT*V+TR`)
  - puis conversion approx en "world" via inversion d'une transform de reference de frame
    (`p_w = R_ref^T * (p_c - T_ref)`)
- Remarque:
  - c'est experimental (pas une decomposition camera/objet parfaite)
  - utile pour test free-roam plus naturel quand `TrackingMode=WorldLocked`
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

## IMPORTANT - travail interrompu en cours

Prototype compile, mais validation fonctionnelle non faite (CLI/UE5).
Il reste des changements potentiels a faire dans:

- `src/r3000/bus.cpp`
  - eventuellement instrumenter stats token-hit/miss par frame pour debug

- `src/gpu/gpu_3d.cpp`
  - verifier le comportement des quads Ridge (majority vote peut etre insuffisant)
  - ajouter logs de qualite token (hints valides par primitive)

- `src/r3000/cpu.cpp`
  - etendre eventuellement propagation token a d'autres ops de move/merge (SWL/SWR, etc.)
  - verifier que clear token agressif ne casse pas des chemins utiles

## Etat git notable avant cette tache

Le repo etait deja "dirty" avec:
- `.gitignore` modifie
- suppressions staged d'artefacts UE5 binaries
- docs ajoutees

Ne pas reset hard.

## Intention technique (rappel)

Ce prototype vise un chemin deterministic "token flow" 100% generique (tous jeux PS1):
- lien GTE->CPU->RAM->DMA2->GPU resolu de maniere causale, pas specifique a un jeu
- pas de dependance principale au decode des coordonnees taggees
- pas de fallback heuristique en mode normal
- Ridge Racer est utilise uniquement comme cas de validation/repro, pas comme cible unique

Si le hit rate token est insuffisant, la prochaine iteration devra etendre la propagation
token sur davantage d'instructions de copie/transfo CPU.

17. Refactor propre: analyse provenance CPU centralisee (nouvelle classe)
- Ajout d'une classe dediee:
  - `src/r3000/cpu_provenance_analyzer.h`
  - `src/r3000/cpu_provenance_analyzer.cpp`
- Integration:
  - `src/r3000/cpu.h`: membre `CpuProvenanceAnalyzer provenance_analyzer_{}`
  - `CMakeLists.txt`: compile `cpu_provenance_analyzer.cpp`
- Principe:
  - cache par `PC` d'une regle de propagation de token (evite de re-analyser chaque fois)
  - regles initiales: `SLL shamt=0`, `ADDU` avec registre zero, `SUBU rt=0`,
    `OR` avec zero/meme registre, `ADDIU imm=0`, `ORI imm=0`
- Nettoyage CPU step:
  - suppression des appels ad-hoc `apply_cached_provenance(...)` dans les opcodes
  - application UNIQUE apres decode/execute, basee sur `wb_valid/wb_reg`
  - le token est applique seulement si regle valide et destination != r0
- Fichier impacte:
  - `src/r3000/cpu.cpp`
- Build:
  - `cmake --build build -j 4` OK
- Objectif:
  - rendre la logique de "bidouille" d'analyse plus propre, extensible et optimisable
  - preparer les prochaines regles sans polluer le switch opcode principal

18. Extension des regles de provenance (iteration perf/coverage)
- Fichier:
  - `src/r3000/cpu_provenance_analyzer.cpp`
- Nouvelles regles:
  - `ADDU` -> `merge_rs_rt` (propage si un seul cote est tokenise, ou si les 2 tokens sont egaux)
  - `OR` -> `merge_rs_rt`
  - `XOR` -> `merge_rs_rt`
  - `SUBU` -> `copy_rs_if_rt_none`
  - `ADDIU` -> `copy_rs` (plus seulement `imm==0`)
- Intention:
  - mieux suivre les tables d'index/offsets dans les boucles CPU avant emission GPU
  - conserver une politique conservative: si conflit de tokens (rs != rt), on n'infere pas
- Build:
  - `cmake --build build -j 4` OK

19. Recuperation 3D sans token face via table SXY->vertex (Gpu3D)
- Constat:
  - `tok_miss` restait stable a 248 (frames 540/600/660), donc les regles CPU seules ne suffisent pas.
- Implementation:
  - `src/gpu/gpu_3d.cpp`:
    - ajout fallback "vertex lookup" dans `push_triangle` et `push_quad`
    - si `face_idx` absent/invalide:
      - lookup de chaque vertex via `gte_3d_->lookup_by_sxy(pack(x,y))`
      - validation: les vertices trouves doivent partager la meme transform
      - si ok: on remplit `DrawCmd3D` directement depuis `GteCacheVertex`
  - `src/gpu/gpu_3d.h`:
    - nouveaux compteurs: `vtx_lookup_hits_`, `vtx_lookup_misses_`
  - logs vblank:
    - ajout `vtx_hit` / `vtx_miss` dans `[GPU3D_VBLANK]`
- Intention:
  - couvrir les polygons 3D qui ne portent pas de token face exploitable, en utilisant
    une correspondance directe 2D->3D deja maintenue dans GTE3D.
- Build:
  - `cmake --build build -j 4` OK

20. Integration du contexte d'appels dans l'analyse CPU (cache par PC+call)
- Motivation:
  - meme code (meme PC) peut etre execute depuis des callsites differents avec des flux
    de donnees differents; un cache par PC seul melange ces cas.
- Changements:
  - `src/r3000/cpu_provenance_analyzer.h/.cpp`
    - `infer_reg_token(pc, call_ctx, instr, reg_tokens)`
    - cle de cache: `(call_ctx << 32) | pc`
  - `src/r3000/cpu.h`
    - ajout etat call-context:
      - `call_ctx_hash_`
      - `call_ctx_stack_[64]`
      - `call_ctx_sp_`
  - `src/r3000/cpu.cpp`
    - reset du call-context dans `Cpu::reset()`
    - push call-context sur:
      - `JAL`
      - `JALR`
      - `BLTZAL/BGEZAL` quand branch prise
    - pop call-context sur:
      - `JR ra`
    - propagation token centralisee utilise maintenant `call_ctx_hash_`
- Build:
  - `cmake --build build -j 4` OK

21. Outil d'analyse GPU3D: decomposition des misses token (sans fallback)
- Objectif:
  - identifier exactement OU casse le lien GTE->GPU, sans masquer via rendu fallback.
- Changements:
  - `src/gpu/gpu_3d.h/.cpp`
    - nouveaux compteurs:
      - `miss_no_hint`: aucun hint token dans le packet polygon
      - `miss_hint_stale`: hint present mais absent du cache face (token stale)
      - `miss_decode_fail`: decode fallback n'a pas produit de face_idx valide
    - ajout dans log `[GPU3D_VBLANK]` de ces 3 colonnes
- Build:
  - `cmake --build build -j 4` OK

22. Squelette runtime "mode game / mode analysis" + refresh a la demande
- Nouvelles classes:
  - `src/emu/psx3d_mode_manager.h`
  - `src/emu/psx3d_mode_manager.cpp`
- Integration Core:
  - `src/emu/core.h/.cpp`
  - API exposee:
    - `set_psx3d_mode(Psx3dRunMode)`
    - `set_psx3d_analysis_enabled(bool)`
    - `request_psx3d_analysis_refresh(reason, scope)`
    - getters `psx3d_mode()/psx3d_analysis_active()`
- Comportement actuel:
  - gestion d'etat centralisee (game vs analysis + autorisation)
  - file d'attente simple d'une requete refresh
  - hook runtime non-intrusif dans `Core::step()`:
    - consomme la requete et log l'evenement (placeholder pour futur analyseur)
- Build:
  - `CMakeLists.txt` ajoute `src/emu/psx3d_mode_manager.cpp`

23. CLI smoke-test + flags PSX3D
- `cli/main.cpp`:
  - nouveaux flags:
    - `--psx3d-mode=game|analysis`
    - `--psx3d-analysis=0|1`
    - `--psx3d-refresh=reason:scope`
- Test execute:
  - `.\lib\Debug\r3000_emu.exe --max-steps=2 --psx3d-analysis=1 --psx3d-mode=analysis --psx3d-refresh=manual:global`
  - Logs verifies:
    - `analysis_enabled=1`
    - `mode=analysis`
    - `refresh queued ...`
    - `analysis refresh request ...` consommee dans `Core::step()`

24. Trace causale des `miss_no_hint` (DMA2 -> dernier writer PC)
- Objectif:
  - identifier les chemins CPU qui produisent les mots GP0 sans token.
- `src/r3000/bus.h/.cpp`:
  - ajoute meta par mot RAM:
    - `ram_face_writer_pc_`
    - accessor `ram_face_writer_pc(paddr)`
  - `set_ram_face_token(...)` enregistre aussi `cpu_pc_` comme dernier writer
  - pendant DMA2 (block + linked-list), pour chaque mot envoye au GPU:
    - si token absent (`kNoFaceToken`), incremente histogramme par writer PC
  - a VBlank: log `DMA2_NOHINT` avec top PCs responsables, puis reset histogramme
- Build:
  - `cmake --build build -j 4` OK
