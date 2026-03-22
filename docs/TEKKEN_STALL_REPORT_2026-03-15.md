## Tekken (Europe) Stall Investigation

### Résumé
- Boot complet via BIOS SCPH-7502, `SCES_000.05` charge `TEKKEN.EXE` mais l'exécution ne bascule jamais sur `0x8007C15C`.  
- Le CPU tourne dans la même boucle `0x80067814‑0x80067860`, qui dépend exclusivement des registres GPUSTAT (`0x1F801814`, via `0x8009ABDC`) et TMR1_COUNT (`0x1F801110`, via `0x8009ABE0`).  
- La logique tente d'initialiser une structure à `0x0009ABDC‑0x0009ACFC`, met à jour `0x8009ABE4/0x8009ABE8`, puis attend que `0x0009ACFC` devienne non nul — or `0x0009ACFC` reste *0* toute la session (watch logs), donc la routine recharge en boucle.  
- Le mini-jeu affiche sprites/sound mais aucune animation “vaisseaux descend” car l’étape qui doit déclencher `0x0009ACFC` n’arrive pas : il s’agit visiblement d’un flag logiciel alimenté par l’écriture `0x0009ACFC`.

### Observations clés
1. **Trace ciblée `0x8006784C–0x80067860`** : `a0` (index) reste autour de 0x7D..0x86, `a1` pointe sur GPUSTAT, `a2` sur TMR1; la routine appelle `0x80063564` puis retourne sur la comparaison `lbu [0x8002A84B+index]`, dont la valeur reste 0x08 ce qui maintient la boucle.  
2. **Watches RAM (`0x0009ABE0/ABE4/ABE8/ACFC`)** : ABE4/ABE8 ne bougent jamais et ACFC reste 0, donc aucun signal n'est transmis à la routine d’attente ; le champ ACFC apparaît bien comme le compteur attendu.  
3. **Registre `t1=0x801FFCFC`** : trace constante, donc cette boucle dépend uniquement d’un flag logiciel (pas d’interruption changeante).  
4. **Stop-on-PC** : la routine n’atteint jamais `0x800684A0`, donc la condition de sortie où `ACFC >= a0` n’est jamais satisfaite; la progression reste fixée au premier test `0x80067838`.

### Recommandations
1. Identifier/activer l’écriture de `0x0009ACFC` (probablement dans le handler `0x800308C0..0x80030910` ou une routine `TMR1`/`GPUSTAT` juste après la transition).  
2. Vérifier que les IRQ/timers sont bien rechargés après la sortie de `SCES_000.05`: les triggers HLE ont été retirés donc le flag risque de ne pas être « rafraîchi ».  
3. Ajouter un trace additionnel CPU/IRQ autour de `0x0009ACFC` pour documenter l’état manquant ; documenter aussi `GPUSTAT bit31` si le flag attend la fin d’un champ.  
4. Mettre à jour `docs/TEKKEN_GENERIC_BOOT_DEBUG_2026-03-14.md` avec ces nouvelles adresses clés pour faciliter le debug futur.

### Logs utiles
- `logs/tekken_regtrace_6784C_67860_2026-03-15.txt` (boucle, a0 index, register dumps)  
- `logs/watch.log` et `watch_range.log` (moniteurs à 0x0009ACFC et plage 0x0009ABDC–0x0009ACFC)  
- `logs/tekken_stop_67800_2026-03-15.txt` (stop-on-pc, confirmations).  

