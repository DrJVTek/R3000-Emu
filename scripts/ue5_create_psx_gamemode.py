"""
UE5 Editor Script: Create PSX GameMode with custom Pawn and PlayerController.
Run from: Tools > Execute Python Script in UE5 Editor.

Creates:
  /Game/PSX/BP_PSXPawn            - Spectator pawn (fly with ZQSD + mouse, no IMC_Default)
  /Game/PSX/BP_PSXPlayerController - PlayerController with IMC_PSXPad mapping context
  /Game/PSX/BP_PSXGameMode        - GameMode tying them together

Why custom:
  - Default GameMode uses IMC_Default which consumes gamepad buttons
  - Our GameMode uses IMC_PSXPad for Xbox→PS1 mapping only
  - SpectatorPawn gives ZQSD+mouse flight without IMC_Default

After running:
  1. World Settings > GameMode Override > BP_PSXGameMode
  2. PIE > Xbox controller = PS1 pad, keyboard+mouse = 3D navigation
"""

import unreal

OUTPUT_DIR = "/Game/PSX"
INPUT_DIR = "/Game/PSX/Input"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
EIL = unreal.EditorAssetLibrary


def ensure_directory(path):
    if not EIL.does_directory_exist(path):
        EIL.make_directory(path)


def create_blueprint(name, parent_class, directory=OUTPUT_DIR):
    """Create a Blueprint asset based on parent_class. Returns (bp, created)."""
    full_path = f"{directory}/{name}"

    if EIL.does_asset_exist(full_path):
        unreal.log(f"  [skip] {full_path} already exists")
        return unreal.load_asset(full_path), False

    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    bp = asset_tools.create_asset(name, directory, unreal.Blueprint, factory)

    if bp:
        EIL.save_asset(full_path)
        unreal.log(f"  [created] {full_path}")
    else:
        unreal.log(f"  [ERROR] Failed to create {full_path}")

    return bp, (bp is not None)


def main():
    unreal.log("=== Creating PSX GameMode (Pawn + PlayerController + GameMode) ===")

    ensure_directory(OUTPUT_DIR)

    # 1) Create Pawn BP based on SpectatorPawn (fly with WASD+mouse, no IMC_Default)
    pawn_bp, _ = create_blueprint("BP_PSXPawn", unreal.SpectatorPawn)

    # 2) Create PlayerController BP
    pc_bp, pc_created = create_blueprint("BP_PSXPlayerController", unreal.PlayerController)

    # If PlayerController was just created, configure it to add IMC_PSXPad
    if pc_created and pc_bp:
        imc_path = f"{INPUT_DIR}/IMC_PSXPad"
        if EIL.does_asset_exist(imc_path):
            imc = unreal.load_asset(imc_path)
            cdo = unreal.get_default_object(pc_bp.generated_class())
            if cdo and imc:
                # Try to set InputMappingContext on the PC defaults
                try:
                    cdo.set_editor_property("input_mapping_contexts", [imc])
                    unreal.log("  PC: IMC_PSXPad set as default mapping context")
                except Exception:
                    unreal.log("  PC: Cannot set IMC via CDO (will be added at runtime by R3000EmuComponent)")
            EIL.save_asset(f"{OUTPUT_DIR}/BP_PSXPlayerController")
        else:
            unreal.log(f"  WARNING: {imc_path} not found! Run ue5_create_psx_inputs.py first")

    # 3) Create GameMode BP
    gm_bp, gm_created = create_blueprint("BP_PSXGameMode", unreal.GameModeBase)

    if gm_bp:
        cdo = unreal.get_default_object(gm_bp.generated_class())
        if cdo:
            # Set DefaultPawnClass = BP_PSXPawn
            if pawn_bp:
                try:
                    cdo.set_editor_property("default_pawn_class", pawn_bp.generated_class())
                    unreal.log("  GM: DefaultPawnClass = BP_PSXPawn")
                except Exception:
                    unreal.log("  GM: Could not set DefaultPawnClass via CDO (set manually in BP editor)")

            # Set PlayerControllerClass = BP_PSXPlayerController
            if pc_bp:
                try:
                    cdo.set_editor_property("player_controller_class", pc_bp.generated_class())
                    unreal.log("  GM: PlayerControllerClass = BP_PSXPlayerController")
                except Exception:
                    unreal.log("  GM: Could not set PlayerControllerClass via CDO (set manually in BP editor)")

            EIL.save_asset(f"{OUTPUT_DIR}/BP_PSXGameMode")

    unreal.log("")
    unreal.log("=== Done! ===")
    unreal.log("")
    unreal.log("NEXT STEPS:")
    unreal.log("  1. VERIFY in BP editor: open BP_PSXGameMode")
    unreal.log("     - Default Pawn Class = BP_PSXPawn (SpectatorPawn)")
    unreal.log("     - Player Controller Class = BP_PSXPlayerController")
    unreal.log("  2. World Settings > GameMode Override > BP_PSXGameMode")
    unreal.log("  3. PIE: Xbox controller = PS1 pad, ZQSD+mouse = fly 3D")
    unreal.log("")
    unreal.log("If CDO set_editor_property failed, open each BP and set manually:")
    unreal.log("  - BP_PSXGameMode > Default Pawn Class > BP_PSXPawn")
    unreal.log("  - BP_PSXGameMode > Player Controller Class > BP_PSXPlayerController")


main()
