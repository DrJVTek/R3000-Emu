# PSX3D Analysis + Profile Spec (V1)

## But

Construire un pipeline 100% generique (tous jeux PS1) pour lier de maniere causale:

- GTE -> CPU -> RAM -> DMA2 -> GPU

Sans fallback visuel en mode normal.  
Ridge Racer est un cas de validation, pas une cible speciale.

---

## Modes runtime (obligatoire)

Le systeme doit exposer 2 modes principaux et 2 niveaux de controle.

### 1) `game` mode

- utilise uniquement les regles/profile deja connus
- pas d'analyse lourde continue
- objectif: perf/stabilite

### 2) `analysis` mode

- active l'instrumentation et la collecte
- peut apprendre de nouvelles regles pendant execution
- peut enrichir le profil `.psx3d`

### 3) Niveau "autorisation d'analyse"

Controle global:

- `analysis_enabled = false`
  - ne lance jamais l'analyse
  - charge seulement le fichier profil existant (si present)
- `analysis_enabled = true`
  - autorise l'analyse (si mode `analysis`)

### 4) Relance d'analyse a la demande

Meme en session en cours, il faut pouvoir relancer l'analyse a tout moment:

- quand de nouveaux chemins GTE apparaissent
- quand on change de scene/track/menu
- quand le taux de miss reste eleve

API conseillee:

- `request_analysis_refresh(reason, scope)`
  - `reason`: `new_gte_code`, `high_miss_rate`, `manual`, etc.
  - `scope`: `global`, `current_pc_region`, `current_ot_window`

---

## Module A - Taint Tracking Integral (ADN memoire)

### 1) Shadow RAM ADN

Pour chaque mot RAM 32-bit, stocker une meta `DnaTag` (64 bits logiques):

- `frame_id` (16 bits)
- `face_idx` (24..32 bits selon limite choisie)
- `vertex_idx` (2..8 bits)
- `kind` (direct/copied/arith/unknown)
- `confidence` (0..255)
- `flags` (conflict, stale, partial)

Implementation conseillee:

- tableau parallele a la RAM (`std::vector<DnaTag>` indexe par `paddr >> 2`)
- zero-cost path si tag `unknown`

### 2) Sources ADN

- `SWC2` (SXY* ou donnees derivees GTE): tag `direct_gte`, confiance max
- copies (`LW` -> reg, `SW` -> mem): conservation du tag
- charges partielles (`LWL/LWR/SWL/SWR`): tag `partial`

### 3) Propagation CPU (reg -> reg)

Ne pas appliquer "base pointer gagne toujours" en dur.  
Utiliser une fusion stricte:

- si 1 seul tag valide -> propager
- si 2 tags identiques -> propager
- si 2 tags differents -> `conflict` + confidence basse
- sinon `unknown`

Clé cache d'analyse:

- `PC + call_context + opcode_shape`

### 4) Resolution finale GPU

Au moment du DMA2 LL vers GP0:

- lire `DnaTag` a l'adresse du mot
- injecter le hint vers `gp0_with_face_hint`
- comptabiliser par raison:
  - `no_tag`
  - `conflict`
  - `stale`
  - `resolved`

---

## Module B - Matrix Root Tracing (camera/object roots)

### 1) Hook declencheur

Intercepter `MTC2` quand destination COP2 touche:

- matrice RT / translation TR (registres GTE 0..11 selon mapping en cours)

### 2) Backtracking

Depuis le registre CPU source:

1. remonter la provenance reg (fenetre d'instructions, avec call_context)
2. identifier dernier `LW/ADDIU/ADDU/...` menant a une base RAM
3. extraire adresse racine candidate

### 3) Validation temporelle

Une racine est valide si:

- mise a jour avec periodicite frame-stable
- coherent avec fenetre VBlank (ex: juste avant draw list ou juste apres IRQ)
- stable sur `N` frames consecutives (configurable)

Sortie:

- `camera_roots[]` (ou `transform_roots[]`) avec confiance + stats

---

## Module C - Heuristique Subdivision UV/Ecran

Objectif: recoller des primitives GPU orphelines (`miss_no_hint`) a un parent GTE.

### 1) Snapshot parent (GTE)

Pour chaque face candidate:

- bbox ecran: `xmin,xmax,ymin,ymax`
- bbox UV: `umin,umax,vmin,vmax` (si texture)
- transform + frame_id

### 2) Cluster orphelins (GPU)

Regrouper une courte sequence de polys sans token:

- proximite temporelle (meme paquet DMA / meme OT bucket)
- coherence texture/clut/texpage

### 3) Test d'inclusion

Un cluster est enfant d'un parent si:

- XY enfant inclus dans bbox XY parent (avec epsilon)
- UV enfant inclus dans bbox UV parent (si texture)
- ordre OT compatible

### 4) Assignation

Si score > seuil:

- assigner `face_idx` parent aux enfants
- marquer `kind=subdiv_relinked` (jamais `direct_gte`)
- confidence derivee du score

Important:

- ce module reste optionnel et traçable
- toute assignation doit etre explicable dans les logs

---

## Fichier de profil `.psx3d` (format V1)

## Objectif

Persist des regles apprises pour accelerer le resolve au demarrage suivant.

## Encodage

- JSON versionne (V1), lisible et diffable
- plus tard possible: binaire compact V2

## Structure proposee

```json
{
  "version": 1,
  "game_id": {
    "exe_name": "SLUS_000.00",
    "exe_crc32": "A1B2C3D4"
  },
  "build_fingerprint": {
    "emu_git": "abcdef12",
    "profile_created_utc": "2026-03-05T12:34:56Z"
  },
  "rules": [
    {
      "type": "dma_word_to_face",
      "pc": "0x80012340",
      "call_ctx": "0x8F11AA22",
      "opcode_shape": "SWC2->SW chain",
      "ram_offset": 16,
      "confidence": 0.97,
      "hits": 14520,
      "misses": 231
    }
  ],
  "camera_roots": [
    {
      "addr": "0x80045A00",
      "kind": "view_matrix",
      "confidence": 0.93,
      "frame_stability": 0.99
    }
  ],
  "heuristics": {
    "subdiv_uv_screen_enabled": true,
    "subdiv_threshold": 0.92
  },
  "exclusions": [
    {
      "pc": "0x8009AB10",
      "reason": "false_positive_conflict"
    }
  ],
  "stats": {
    "resolved": 0,
    "no_tag": 0,
    "conflict": 0,
    "stale": 0
  }
}
```

---

## Chargement au demarrage

1. Identifier le jeu (`exe_name`, CRC).  
2. Charger le `.psx3d` correspondant.  
3. Valider `version` + compatibilite build.  
4. Installer les regles en cache (lecture seule).
5. Appliquer politique mode/autorisation:
   - si `analysis_enabled=false`: rester en `game` strict
   - si `analysis_enabled=true` et mode `analysis`: collecte active
6. En runtime, mettre a jour compteurs + nouvelles candidates (si analyse autorisee).  
7. Sauvegarder en fin de session si gain net.

---

## Garde-fous

- Aucune regle profilee ne doit bypass la verification runtime minimale.
- En cas de conflit massif, auto-disable de la regle fautive.
- Logs obligatoires pour toute decision non triviale (`why`, `score`, `source`).
- Une relance d'analyse ne doit pas invalider brutalement les regles stables:
  - marquer `candidate` puis promotion apres validation multi-frame.

---

## Plan d'implementation conseille

1. Ajouter `DnaTag` RAM + stats (sans changer le rendu).  
2. Ajouter resolve DMA2 -> GPU avec motifs d'echec explicites.  
3. Ajouter call-context complet dans le cache d'analyse (deja partiellement en place).  
4. Ajouter serialization `.psx3d` V1 (save/load).  
5. Ajouter module C (subdiv) derriere flag runtime.
