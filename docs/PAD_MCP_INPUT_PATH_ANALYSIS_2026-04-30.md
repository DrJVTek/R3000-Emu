# PAD / MCP input path analysis - 2026-04-30

## TL;DR

Le modele correct est **hardware direct, non-HLE**:

```text
UE physical pad  -> Core local pad mask --+
                                          +-> active-low AND -> Bus slot mask -> SIO0 serial protocol -> BIOS/game
MCP pad tools    -> Core MCP pad mask ----+

CLI auto-input   -> Core local pad mask --+
CLI MCP tools    -> Core MCP pad mask ----+
```

Regle centrale: le MCP ne doit pas "prendre" le pad local par defaut. Le pad UE
physique et le MCP sont deux sources hardware independantes. Elles sont
melangees dans `Core` par `local_mask & mcp_mask` parce que le pad PS1 est
actif-bas (`0 = pressed`, `1 = released`). Donc si une source appuie, le jeu le
voit; le bouton n'est relache que quand les deux sources relachent.

Le dernier log UE disponible (`E:\Projects\github\Live\PSXVR\logs\system.log`,
2026-04-30 02:18) montre que Ridge Racer poll bien SIO0, mais que SIO0 ne voit
que `0xFFFF`. Dans ce run, l'input n'est donc pas arrive en masque non-idle au
Bus avant le latch SIO0.

Attention importante: le fichier source `PSXEmulatorComponent.cpp` a ete modifie
a `02:22`, apres le log UE `02:18`, et la DLL UE date de `02:11`. Les nouveaux
logs `PadInput poll #...` ne peuvent donc pas etre utilises pour interpreter ce
run tant que l'integration UE n'a pas ete reconstruite/chargee.

Update 02:48: le nouveau run prouve que le pad UE physique arrive bien a SIO0:
`PadInput buttons=...` puis `SIO0 xfer START ... latched_btns!=0xFFFF`. Le
probleme restant est donc plus loin: aucun `SIO0_PAD_READ`, aucun
`SIO0_PAD_PHASE`, aucun `SIO0 xfer COMPLETE`.

Update 03:00: hypothese invalidee/rejetee. Un patch temporaire avait tente de
router aussi les acces SIO0 32-bit (`read_u32/write_u32`) vers les helpers pad,
avec le marqueur `BUS source v57 (sio0_u32_pad_path)`. Il n'a pas explique la
regression et a ete retire pour ne pas polluer l'analyse. Le marqueur attendu
revient a `BUS source v56 (sio0_pad_trace)`.

Point critique: les logs UE ont montre `BUS source v57`, alors que le DLL
principal du plugin contenait encore `BUS source v56` et les strings
`SIO0_PAD_PHASE` / `SIO0_MMIO`. Ce n'est pas un probleme de confiance/rebuild:
avec Live Coding, la verite runtime est l'ensemble DLL principal + patchs
charges. Utiliser `scripts/ue_runtime_markers.ps1` pour lire le `BuildId`, les
modules charges et les marqueurs embarques dans chaque module avant de conclure.

Update 03:10: l'Unreal lance ne charge pas le DLL depuis
`R3000-Emu/integrations/...`, mais depuis
`E:\Projects\github\Live\PSXVR\Plugins\R3000Emu\Binaries\Win64`. Les modules
charges sont le DLL principal plus les patchs Live Coding
`UnrealEditor-R3000EmuRuntime.patch_0..3.exe`. Le DLL principal contient encore
`BUS source v56`, mais `patch_2.exe` et `patch_3.exe` contiennent `BUS source
v57` + `sio0_u32_pad_path`. Donc le dernier test UE a bien execute le patch
temporaire v57. Le check canonique est maintenant:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ue_runtime_markers.ps1
```

Exemple observe: `BuildId=47537391`, DLL principal `BUS source v56`, patchs
Live Coding `patch_2/patch_3` en `BUS source v57`. C'est compatible avec Live
Coding; il faut juste raisonner sur les marqueurs des modules charges, pas sur
un seul fichier DLL inspecte au hasard.

Update 03:30: le run Live Coding suivant prouve que `patch_4.exe` porte bien
`BUS source v56 (sio0_pad_trace)`. Le pad UE arrive toujours (`PadInput
buttons`) et SIO0 latch des masques non-idle (`SIO0 xfer START ...
latched_btns!=0xFFFF`). Mais les logs profonds (`SIO0_PAD_PHASE/READ/ACK/MMIO`)
restent absents parce que leurs compteurs etaient `static` dans les fonctions:
ils survivent au process Unreal et au Live Coding. Fix applique: les compteurs
SIO0 diagnostic sont maintenant des membres de `Bus`, avec marqueur
`BUS source v58 (sio0_diag_counters_per_bus)`. Le log UE `PadInput buttons`
decode aussi les noms (`names=cross,start,left,...`) pour eviter de deviner les
masques hex.

## Contraintes non-negociables

- Pas de HLE pour les inputs dans cette passe.
- Pas de touches clavier hardcodees dans une couche haute pour contourner le pad.
- Pas d'ownership MCP exclusif par defaut.
- Le chemin doit rester commun entre UE et CLI des que possible.
- Le PAD direct et le multitap doivent reutiliser le meme stockage hardware bas
  niveau (`slot 0..7`), pas deux systemes separes.
- Ne pas rollback sans analyse. Les comparaisons Git servent a comprendre, pas a
  ecraser le travail courant.

## Chemin UE pad physique

Fichiers:

- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/PSXEmulatorComponent.cpp`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/PSXEmulatorComponent.h`

Chemin attendu:

```text
UPSXEmulatorComponent::TickComponent()
  -> PollPadInput()
     -> GetWorld()->GetFirstPlayerController()
     -> PC->IsInputKeyDown(EKeys::Gamepad_*)
     -> PC->GetInputAnalogKeyState(Gamepad_LeftX/Y) optional D-pad
     -> Core_->set_pad_local_buttons_for_slot(LocalPadSlot, Buttons)
     -> Core::apply_effective_pad_buttons(slot)
     -> Bus::set_pad_slot_buttons(slot, effective)
```

Points verifies dans le code courant:

- `PollPadInput()` est appele depuis `TickComponent()` quand `bRunning && Core_`
  (`PSXEmulatorComponent.cpp` autour de 1272-1279).
- `bForwardLocalPadInput` est maintenant seulement un switch utilisateur/debug;
  il ne doit pas etre change par MCP.
- `bMcpPadInputOwned_` est loggue mais ne bloque plus le polling local.
- Le chemin courant lit directement `EKeys::Gamepad_*` via `APlayerController`.
- Le stick gauche peut etre mappe en D-pad PS1 par seuil analogique.
- Le masque final est envoye a `Core_->set_pad_local_buttons_for_slot(Slot, Buttons)`.

Point connu-good historique:

- Commit `0e69171` (`Disable pawn input, add pad diagnostic logging`) utilisait
  `PC->SetIgnoreMoveInput(true)` et `PC->SetIgnoreLookInput(true)`.
- Il n'utilisait pas `Pawn->DisableInput(PC)`.
- Le chemin connu-good finissait par `Core_->set_pad_buttons(Buttons)`, donc
  slot 0 direct.

Risque actuel:

- La propriete s'appelle encore `bDisablePawnInputForPad`, mais le code correct
  ne doit pas appeler `Pawn->DisableInput`. Le nom est trompeur; le comportement
  voulu est plutot "PlayerController ignore move/look".
- Si un Blueprint existant a serialize `LocalPadSlot != 0`, Ridge Racer direct
  pad ne verra rien en slot 0. Le nouveau log `PadInput poll` expose `local_slot`.
- Les assets Enhanced Input (`IA_*`, `IMC_PSXPad`) sont charges/ajoutes, mais le
  polling reel utilise encore `IsInputKeyDown` direct. Si UE ne remonte plus les
  `EKeys::Gamepad_*` dans ce contexte, il faudra soit lire les valeurs Enhanced
  Input, soit ajouter un diagnostic qui compare raw keys vs actions.

## Chemin UE MCP

Fichiers:

- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/PSXMcpServerComponent.cpp`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/PSXMcpServerComponent.h`
- `src/emu/core_mcp_backend.cpp`
- `src/emu/mcp_server.cpp`

Chemin attendu:

```text
MCP TCP client
  -> McpServer::dispatch_tool_call()
  -> CoreMcpBackend::{set,hold,tap,release}_pad*
  -> Core::set_pad_mcp_buttons_for_slot(slot, mask)
  -> Core::apply_effective_pad_buttons(slot)
  -> Bus::set_pad_slot_buttons(slot, effective)
  -> SIO0
```

Points verifies dans le code courant:

- Les tools pad acceptent `slot` optionnel `0..7`.
- `emu.get_pad_state` retourne `buttons_mask`, `local_mask`, `mcp_mask` et les
  noms de boutons.
- `emu.set_pad_state` ecrit le masque MCP complet.
- `emu.hold_pad_*` maintient un ou plusieurs boutons jusqu'a release.
- `emu.release_pad*` relache un slot.
- `emu.tap_pad_*` restaure le masque MCP precedent apres la fenetre de hold.
- `emu.step_with_pad_observation` garde l'appui pendant `hold_steps` et
  `observe_steps`, puis observe la scene vectorielle avant restauration.
- Le `PSXMcpServerComponent` demarre un serveur TCP, cree un `CoreMcpBackend`
  partage, puis protege les appels MCP en pausant le worker UE via
  `PauseWorkerForMcpAccess`.

Point important:

- `FPsxMcpCallGuard::BeginPadOwnershipForTool()` documente bien le mode mixer
  hardware par defaut et n'appelle plus `AcquirePadOwnership()`.
- `bTakePadOwnership` existe encore en header comme compat/deprecated, mais il
  n'est pas utilise dans `StartServer()` pour couper le pad local.

Risque actuel:

- `AcquirePadOwnership()` existe encore et appelle `SetMcpPadInputOwned(true)`,
  mais il n'est plus appele par le chemin normal. A garder en tete si une future
  modification le reactive par accident.
- `CoreMcpBackend` en header dit que les appels ne sont pas thread-safe contre
  `Core`. Le chemin UE compense par `PauseWorkerForMcpAccess`, qui met
  `bWorkerPaused_` puis attend `!bWorkerInCoreStep_`. Ce verrouillage est une
  hypothese importante a conserver.

## Chemin CLI MCP

Fichiers:

- `cli/main.cpp`
- `src/emu/core_mcp_backend.cpp`
- `src/emu/mcp_server.cpp`

Chemin attendu:

```text
r3000_emu.exe --mcp-stdio / --mcp-tcp=N
  -> CoreMcpBackend(core, cli)
  -> McpServer
  -> tools pad
  -> Core::set_pad_mcp_buttons_for_slot()
  -> Bus::set_pad_slot_buttons()
```

Preuve actuelle:

- `logs/mcp_pad_hold_test.out` montre que `hold_pad_named_buttons` sur slot 0
  passe `start,cross` en `effective_mask=49143`.
- Le meme test montre `release_pad_named_buttons` puis `release_pad`.
- `logs/mcp_multitap_slot4_test.out` existe pour un slot multitap, ce qui valide
  au moins le stockage/API slot-based cote MCP.

Limite actuelle:

- Le test CLI Ridge Racer avec `--auto-input` a bien ecrit le masque pad, mais le
  run CLI n'a pas atteint le meme polling SIO0 que UE dans la fenetre testee. Le
  CLI valide donc le stockage/mix pad, pas encore la reaction gameplay Ridge.

## Chemin CLI auto-input

Fichier:

- `cli/main.cpp`

Chemin actuel:

```text
--auto-input
  -> chaque nouveau VBlank: calcule un masque actif-bas
  -> core.set_pad_local_buttons(pad)
  -> Core::set_pad_local_buttons_for_slot(0, pad)
  -> Core mix local&mcp
  -> Bus slot 0
```

Ce chemin est utile pour tester le stockage local `Core` sans UE. Il n'est pas
un vrai controleur physique et ne prouve pas que `APlayerController` remonte les
boutons dans UE.

## Chemin commun Core / Bus / SIO0

Fichiers:

- `src/emu/core.cpp`
- `src/emu/core.h`
- `src/r3000/bus.cpp`
- `src/r3000/bus.h`

Modele Core:

```text
pad_local_buttons_[slot] = source UE physique ou CLI auto-input
pad_mcp_buttons_[slot]   = source MCP
effective                = local & mcp
bus_->set_pad_slot_buttons(slot, effective)
```

Points verifies:

- `Core::Core()` initialise les arrays local/MCP a `0xFFFF`.
- `Core::set_pad_local_buttons_for_slot()` et
  `Core::set_pad_mcp_buttons_for_slot()` appellent tous les deux
  `apply_effective_pad_buttons(slot)`.
- `apply_effective_pad_buttons()` loggue `set_pad slot=... effective=...` quand
  le masque effectif n'est pas `0xFFFF`.
- `Bus` stocke les slots pad dans un global atomic `g_pad_buttons[8]` pour eviter
  les problemes de layout/hot reload UE.
- `Bus::Bus()` reinitialise tous les slots a `0xFFFF`.

Modele SIO0:

```text
game writes JOY_DATA/JOY_CTRL
  -> SIO0 phase 0 recoit device byte 0x01..0x04
  -> latch pad_slot_buttons(slot) dans sio0_latched_pad_buttons_[]
  -> phases 1/2 renvoient ID/access
  -> phases 3/4 renvoient low/high buttons latched
```

Points verifies:

- Le latch SIO0 prend un snapshot de tous les slots au debut de la transaction.
- Les phases 3/4 renvoient le masque latched, pas le masque live.
- Les logs `SIO0 xfer #... START` affichent `latched_btns` et `live_btns`.
- Les logs `SIO0_PAD_PHASE`, `SIO0_PAD_READ`, `SIO0_PAD_ACK`,
  `SIO0 xfer COMPLETE` ne sortent que si au moins un bouton est non-idle.

Risque actuel:

- Le code appelle encore `deliver_events_for_class(..., 0xF0000009)` a la fin
  d'un transfert pad. Ce n'est pas le sujet du dernier echec, mais c'est une zone
  a garder sous surveillance car le projet a deja interdit la double-delivrance
  manuelle pour VBlank/CDROM.

## Multitap

Etat courant:

- `Bus::kPadSlotCount = 8`.
- MCP et Core savent adresser `slot 0..7`.
- Le SIO0 reconnait `0x01..0x04` comme selection de slots A-D.
- Une methode multitap minimale arme une lecture longue apres un acces slot A
  avec TAP byte `0x01`, puis renvoie quatre blocs digital-pad.

Ce que ca garantit:

- On n'a pas deux chemins separes "pad direct" et "tape HLE".
- Les boutons multitap viennent des memes masques hardware `Core/Bus`.

Ce que ca ne garantit pas encore:

- Le protocole multitap complet de Moto Racer 1/2/3 n'est pas valide.
- Il faut tester les sequences exactes du jeu et verifier les bytes lus par le
  BIOS/game avant de declarer le multitap compatible.

## Lecture des derniers logs UE

Log inspecte:

- `E:\Projects\github\Live\PSXVR\logs\system.log`
- timestamp: 2026-04-30 02:18

Faits observes:

- `BUS source v56 (sio0_pad_trace)` est present: le Bus charge contient bien la
  version de diagnostic SIO0.
- Le worker UE tourne.
- Ridge Racer poll SIO0: on voit `SIO0 xfer #4500` a `#6000`.
- Tous les `SIO0 xfer START` visibles sont:
  `latched_btns=0xFFFF live_btns=0xFFFF`.
- Aucun log `set_pad`.
- Aucun log `PadInput`.
- Aucun log `SIO0_PAD_PHASE/READ/ACK/COMPLETE` non-idle.
- Aucun log MCP.

Interpretation stricte:

- Pour ce run, SIO0 fonctionne assez pour etre polle par Ridge Racer.
- Pour ce run, le Bus ne contient pas de masque pad non-idle au moment du latch.
- Le symptome observe n'est donc pas "le jeu lit un bouton mais l'ignore"; c'est
  "SIO0 ne recoit que all-released".

Limite de l'interpretation:

- Les nouveaux logs `PadInput poll #...` ont ete ajoutes en source apres ce run.
  Leur absence dans ce log ne prouve pas que `PollPadInput()` n'est pas appele.
- En revanche, l'absence de `set_pad` pendant un appui utilisateur reste un bon
  indice que rien de non-idle n'a atteint le mix Core/Bus dans cette build.

## Lecture du run UE suivant (02:48)

Log inspecte:

- `E:\Projects\github\Live\PSXVR\logs\system.log`
- timestamp: 2026-04-30 02:48

Faits observes:

- `PadInput poll #1..#8` apparait avec `forward=1`, `mcp_owned=0`,
  `local_slot=0`.
- `PadInput buttons=0xFFDF`, `0xFF7F`, `0xBFFF`, etc. apparait: le pad physique
  UE est bien lu.
- `SIO0 xfer START` apparait ensuite avec `latched_btns` et `live_btns` non-idle.
- Toujours aucun `SIO0_PAD_READ`, aucun `SIO0_PAD_PHASE`, aucun
  `SIO0 xfer COMPLETE`.

Nouvelle interpretation:

- La rupture n'est plus `UE input -> Core/Bus`.
- La rupture est `SIO0 START -> DATA/STAT reads/writes consommes`.
- L'hypothese "acces SIO0 32-bit non route" a ete testee temporairement puis
  rejetee: le run suivant chargeait encore des patchs Live Coding v57 et n'a pas
  donne de preuve suffisante. Cette hypothese ne doit plus etre consideree comme
  un fix tant qu'un log propre n'a pas montre des acces JOY 32-bit reels.
- Le prochain diagnostic doit d'abord lire les marqueurs runtime avec
  `scripts/ue_runtime_markers.ps1` et savoir si le code actif est porte par le
  DLL principal, par un patch Live Coding, ou par les deux. Le prochain marqueur
  attendu apres Live Coding est `BUS source v58 (sio0_diag_counters_per_bus)`.

## Sequence de logs attendue au prochain run

Avec un runtime UE dont les marqueurs sont identifies, un appui Start/Cross
devrait donner:

```text
[BUS]  BUS source v58 (sio0_diag_counters_per_bus)
[CORE] PadInput poll #1 forward=1 mcp_owned=0 local_slot=0 ...
[CORE] PadInput: PlayerController ignores move/look ...
[CORE] PadInput buttons=0xBFF7 analog=(...) slot=0
[BUS]  set_pad slot=0 effective=0xBFF7 local=0xBFF7 mcp=0xFFFF ...
[BUS]  SIO0 xfer #... START: slot=0 ... latched_btns=0xBFF7 live_btns=0xBFF7
[BUS]  SIO0_MMIO_RD16/8 ...
[BUS]  SIO0_PAD_PHASE ...
[BUS]  SIO0_PAD_READ data=...
[BUS]  SIO0 xfer COMPLETE: btns=0xBFF7 lo=... hi=...
```

Diagnostic par rupture:

- Pas de `PadInput poll`: Tick UE n'execute pas le composant, build pas a jour,
  ou `bRunning/Core_` faux.
- `PadInput poll` mais pas `PadInput buttons`: `APlayerController` ne voit pas
  les `EKeys::Gamepad_*`; suspect UE input mode / Enhanced Input / focus.
- `PadInput buttons` mais pas `set_pad`: probleme Core call / slot / build.
- `set_pad slot=0` mais SIO0 reste `0xFFFF`: probleme Bus global storage ou
  mauvais slot lu.
- `SIO0 START` non-idle mais pas de `SIO0_PAD_PHASE`: suspect runtime/binaire
  non aligne ou `sio0_do_transfer()` pas celui du source attendu.
- `SIO0_PAD_PHASE` non-idle mais pas de `SIO0_PAD_READ`: le BIOS/jeu n'a pas lu
  la reponse data, ou la transaction est interrompue avant consommation.
- SIO0 lit les bons bytes mais le jeu ne reagit pas: chercher cote routine
  BIOS/jeu, buffer pad, IRQ/ACK, ou protocole multitap.

## Hypotheses prioritaires

1. **Diagnostics SIO0 caches par des compteurs `static` persistants.**
   Confirme par `SIO0 xfer #10000` apres relance: les compteurs survivaient au
   process Unreal et au Live Coding. Fix source v58: compteurs membres de `Bus`.

2. **SIO0 START non-idle mais transaction non consommee jusqu'au read.**
   A verifier dans un runtime propre avec `SIO0_PAD_PHASE`, `SIO0_PAD_ACK`,
   `SIO0_PAD_READ` et `SIO0 xfer COMPLETE`.

3. **Mauvaise lecture du runtime UE / Live Coding.**
   A controler avec `scripts/ue_runtime_markers.ps1`; ce n'est pas un bug Live
   Coding, il faut juste lire les marqueurs des modules charges.

4. **Le chemin UE `APlayerController -> EKeys::Gamepad_*` ne voit plus le pad.**
   Moins probable parce que `PadInput buttons=...` apparait. Le log affiche
   maintenant aussi les noms de boutons.

4. **Blueprint/instance serializee avec mauvais slot ou flags.**
   A verifier via le log `PadInput poll`: `forward=1`, `local_slot=0`,
   `disable_pawn=1` attendu pour Ridge Racer direct pad.

5. **Le MCP n'est probablement pas la cause directe du pad physique dans le code
   courant.**
   Le serveur UE ne coupe plus `bForwardLocalPadInput` et n'appelle plus
   l'ownership par defaut. Mais il reste a verifier dans la build chargee.

6. **Le multitap est structurellement sur le bon chemin, mais pas encore prouve
   gameplay.**
   Moto Racer doit etre un test separe avec logs SIO0 longs.

## Actions recommandees avant nouveau code

1. Executer `scripts\ue_runtime_markers.ps1` et noter le `BuildId` + les
   marqueurs `BUS source vXX` des modules charges. Attendu: v58 apres Live
   Coding.
2. Lancer Ridge Racer avec `CoreLogLevel=info` ou `warn` suffit pour les logs
   ajoutes via `emu::logf(... warn, ...)`.
3. Appuyer Start/Cross et chercher la premiere rupture dans la sequence de logs
   attendue.
4. Si rupture `PadInput poll` -> pas `PadInput buttons`, ajouter ensuite un
   diagnostic compare:
   - raw `PC->IsInputKeyDown(EKeys::Gamepad_*)`
   - `GetInputAnalogKeyState`
   - valeurs Enhanced Input action si disponibles
5. Si rupture `set_pad` -> SIO0, tester slot 0 explicitement via MCP:
   - `emu.hold_pad_named_buttons {"names_csv":"start","slot":0}`
   - `emu.get_pad_state {"slot":0}`
   - puis observer `SIO0 xfer START`.
6. Tester multitap seulement apres retour du pad direct, sinon on melange deux
   problemes.

## Diagnostic v58 Ridge Racer: ACK high byte perdu

Run UE Ridge Racer, logs `system.log` du 2026-04-30:

- Le pad humain arrive bien dans UE: `PadInput buttons=0xBFFF names=cross`,
  `0xFFDF names=right`, etc.
- Le masque arrive bien dans SIO0: `SIO0_PAD_PHASE ... latched=0xBFFF` puis
  `latched=0xFFDF`.
- La transaction pad consommee par le jeu est:
  - `0x01 -> 0xFF`
  - `0x42 -> 0x41`
  - `0x00 -> 0x5A`
  - `0x00 -> low byte`
- La phase suivante `phase=4->0`, qui devrait envoyer le high byte, n'apparait
  pas pendant l'appui.

Interpretation:

- Ce n'est pas un probleme de mapping UE: les noms de boutons et les masques
  sont corrects.
- Ce n'est pas un probleme MCP/HLE: la voie est bien UE/Core/Bus/SIO0 hardware.
- Le jeu ou le BIOS deselectionne le pad avant de recevoir l'ACK qui lui ferait
  envoyer le byte suivant. Pour les boutons high byte (`cross`, `square`,
  `circle`, `triangle`, `L/R`), ca rend l'input invisible au gameplay.
- Le worker UE tickait les peripheriques par paquets hardcodes de `1024` cycles,
  alors que l'ACK SIO0 pad est programme a environ `450` cycles. Le batch peut
  donc livrer l'ACK trop tard.

Fix source applique:

- Dans `UPSXEmulatorComponent` worker, remplacer le batch hardcode `1024` par
  `EffectiveBusTickBatch(Owner->bThreadedMode, Owner->BusTickBatch)`.
- Garder `BusTickBatch=1` pour Ridge Racer/Moto Racer/pad direct.
- Ne pas corriger ce symptome par HLE, par mapping clavier, ou par injection
  directe dans un buffer pad BIOS: le bon fix reste le timing SIO0 hardware.

## Diagnostic v59 Ridge Racer: poll court du high byte

Le fix batch/ACK n'a pas suffi sur le run UE suivant: le runtime chargeait bien
`BUS source v58`, le pad humain arrivait bien au core puis a SIO0, mais Ridge
Racer continuait a repeter seulement quatre octets:

```text
0x01 -> 0xFF
0x42 -> 0x41
0x00 -> 0x5A
0x00 -> low byte
```

Le cinquieme transfert `phase=4->0`, qui envoie normalement le high byte du
pad digital, n'etait jamais observe pendant l'appui. Resultat direct: le D-pad
bas peut etre visible (`left/right/down/up`), mais les boutons dans l'octet haut
(`start`, `cross`, `square`, `circle`, `triangle`, `L/R`) restent invisibles.

Fix source applique:

- Marqueur runtime: `BUS source v59 (sio0_short_poll_high_byte)`.
- Quand SIO0 renvoie le low byte d'un packet digital pad non-multitap, le bus
  garde le high byte du meme snapshot.
- Si le BIOS/jeu lit ensuite `JOY_DATA` sans `RXRDY` et sans envoyer un
  cinquieme dummy byte, le bus retourne ce high byte une seule fois.
- Logs attendus si ce chemin est utilise: `SIO0_PAD_SHORT_HIGH` puis
  `SIO0 xfer COMPLETE(short-read)`.
- Cette correction reste dans le chemin `JOY_DATA`/SIO0: pas de mapping clavier,
  pas d'injection RAM jeu, pas de voie HLE separee. Les valeurs viennent
  toujours du stockage pad commun local + MCP + slots.

Affinage suivant les logs UE:

- Le runtime v59 etait bien charge, et `short_high=1` apparaissait deja, donc
  le fallback de lecture fonctionnait.
- Mais ce high byte etait encore livre comme "lecture a vide" (`rxrdy_before=0`),
  ce qui peut etre insuffisant pour certaines routines BIOS/jeu qui attendent
  un vrai byte present dans le RX FIFO.
- Correction suivante: `SIO0_PAD_SHORT_HIGH_QUEUE`. A la fin de l'ACK de
  l'octet bas (phase 4), le high byte latche est maintenant pousse dans le RX
  avec `RXRDY=1`, puis la lecture suivante le consomme comme un byte normal.
- Le fallback `..._FALLBACK` n'est conserve qu'en secours si un poll path lit
  encore `JOY_DATA` sans consommer ce RX.
- Affinage suivant: le runtime Ridge Racer montre aussi un cas encore plus
  court, ou `SELECT` retombe avant meme la fin de cet ACK. Le bus preserve donc
  aussi le high byte sur ce front `SELECT 1->0` (`SIO0_PAD_SHORT_HIGH_SELECT`)
  quand la transaction etait en phase 4. Cela garde le comportement dans la
  couche SIO0/JOY_DATA, avec le meme snapshot latche, sans chemin HLE separe.

## Notes pour le futur

- Le code pad doit rester mutualise au niveau `Core/Bus`. UE et CLI ne doivent
  differer que par la source qui produit le masque.
- Le bon endroit pour "jouer via LLM" est MCP -> `Core::set_pad_mcp_*`, pas UE
  input synth ou clavier.
- Le bon endroit pour "jouer au pad humain" est UE -> `Core::set_pad_local_*`.
- Les deux peuvent etre actifs en meme temps.
- Le document de reference principal reste `docs/DEBUG_UE5_STUCK.md`, mais ce
  fichier doit servir de carte specifique pour toute regression input pad/MCP.
