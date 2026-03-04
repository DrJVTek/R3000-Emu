# Théorie: Quads 3D dégénérés (Ridge Racer gameplay)

> Date: 2026-03-04 | Branche: `002-ue5-3d-materials`
> STATUS: **Théorie non-implémentée** — à valider après résolution de la régression

## Problème observé (CLI `--3d-diag`)

Les 1120 triangles 3D du gameplay ont **tous 2 vertices identiques → aire = 0 → invisibles**.

```
TRI[14] face_idx=465 quad=1 half=0
  v[0] model=(-560,28,-240)   ← UNIQUE
  v[1] model=(-560,35,-280)   ← SHARED
  v[2] model=(-560,35,-280)   ← MÊME QUE V1 → DÉGÉNÉRÉ!

TRI[15] face_idx=465 quad=1 half=1
  v[0] model=(-560,35,-280)   ← SHARED
  v[1] model=(-520,35,-240)   ← de face_B
  v[2] model=(-560,35,-280)   ← MÊME QUE V0 → DÉGÉNÉRÉ!
```

**Résultat**: BBox 3D = X=[124..131] Y=[-56..56] Z=[-40..40] mais 100% des triangles sont plats.

## Cause racine

Ridge Racer traite sa grille de terrain par **arêtes**, pas par faces:
- Chaque RTPT charge 2 vertices uniques + 1 dupliqué (V1 == V2 dans le GTE)
- Les quads GP0 assemblent des vertices de RTPT DIFFÉRENTS
- Le face_cache stocke les 3 verts du MÊME RTPT → face[1]==face[2] → tri dégénéré

### Preuve

```
Face cache idx=465: verts = {(-560,28,-240), (-560,35,-280), (-560,35,-280)}
                                 V0 ≠ V1            V1 == V2 !!

Quad cache: hit=0 miss=560 (aucun quad détecté entre RTPTs consécutifs)
```

Les RTPTs consécutifs ne partagent PAS 2 vertices (condition pour la détection quad),
car chaque RTPT n'a que 2 uniques et ils ne se chevauchent pas.

### Mapping GP0 quad

GP0 reçoit les tagged SXY (après interception MFC2 → shadow GTE):
- V0 = carrier(face_A)  screen=(9864,8200)
- V1 = carrier(face_A)  screen=(9864,8200)  ← MÊME valeur que V0!
- V2 = carrier(face_B)  screen=(9872,8200)
- V3 = carrier(face_B)  screen=(9872,8200)  ← MÊME que V2!

Le jeu n'utilise PAS la valeur reference (SXY1=8192,8192).
SXY0 == SXY2 dans le tagging différentiel → le jeu a UNE seule valeur par RTPT.

## Fix proposé

Dans `push_quad()`, quand `face[1]==face[2]` (pattern dégénéré):

1. **tri1(V0,V1,V2)**:
   - V0 → face_A[0] (vertex unique)
   - V1 → face_A[1] (vertex partagé)
   - V2 → décoder carrier de V2 → face_B[0] (vertex unique de face_B)

2. **tri2(V1,V3,V2)**:
   - V0 → face_A[1] (vertex partagé)
   - V1 → face_B[1] (vertex partagé de face_B)
   - V2 → face_B[0] (vertex unique de face_B)

Résultat: 4 vertices uniques par quad, 2 triangles non-dégénérés.

## Données de référence

- PS logo: 278 tris, 0 dégénérés, BBox X=[479..650] — **fonctionne**
- Gameplay: 1120 tris (560 quads), TOUS dégénérés, BBox X=[124..131]
- RT=[4096,0,0 / 0,0,-4096 / 0,4096,0] TR=(0,0,1280) — rotation 90° + translation
- Le logo PS utilise des triangles (pas de quads) → face cache 3 verts uniques → OK
