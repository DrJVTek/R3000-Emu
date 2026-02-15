"""
UE5 Editor Script: Create PS1 Controller InputAction assets + InputMappingContext.
Run from: Tools > Execute Python Script (or Python REPL) in UE5 Editor.

Creates:
  /Game/PSX/Input/IA_Pad*       - 16 InputAction assets (bool/Digital)
  /Game/PSX/Input/IMC_PSXPad    - InputMappingContext with Xbox controller bindings

Keyboard is NOT mapped here so ZQSD + mouse remain free for UE5 3D navigation/debug.
"""

import unreal

# -- Configuration -------------------------------------------------------------
OUTPUT_DIR = "/Game/PSX/Input"

# (asset_name, bit_index, gamepad_key)
# Xbox controller layout -> PS1 pad mapping:
#   A = Cross, B = Circle, X = Square, Y = Triangle
#   LB = L1, RB = R1, LT = L2, RT = R2
#   DPad = DPad, Start(Menu) = Start, Back(View) = Select
#   LS click = L3, RS click = R3
PAD_BUTTONS = [
    ("IA_PadSelect",    0,  "Gamepad_Special_Left"),
    ("IA_PadL3",        1,  "Gamepad_LeftThumbstickButton"),
    ("IA_PadR3",        2,  "Gamepad_RightThumbstickButton"),
    ("IA_PadStart",     3,  "Gamepad_Special_Right"),
    ("IA_PadUp",        4,  "Gamepad_DPad_Up"),
    ("IA_PadRight",     5,  "Gamepad_DPad_Right"),
    ("IA_PadDown",      6,  "Gamepad_DPad_Down"),
    ("IA_PadLeft",      7,  "Gamepad_DPad_Left"),
    ("IA_PadL2",        8,  "Gamepad_LeftTrigger"),
    ("IA_PadR2",        9,  "Gamepad_RightTrigger"),
    ("IA_PadL1",       10,  "Gamepad_LeftShoulder"),
    ("IA_PadR1",       11,  "Gamepad_RightShoulder"),
    ("IA_PadTriangle", 12,  "Gamepad_FaceButton_Top"),
    ("IA_PadCircle",   13,  "Gamepad_FaceButton_Right"),
    ("IA_PadCross",    14,  "Gamepad_FaceButton_Bottom"),
    ("IA_PadSquare",   15,  "Gamepad_FaceButton_Left"),
]

# -- Helpers -------------------------------------------------------------------
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
EIL = unreal.EditorAssetLibrary


def ensure_directory(path):
    """Create directory if it doesn't exist."""
    if not EIL.does_directory_exist(path):
        EIL.make_directory(path)


def create_input_action(name):
    """Create a bool InputAction asset and return it."""
    full_path = f"{OUTPUT_DIR}/{name}"

    # If it already exists, load and return
    if EIL.does_asset_exist(full_path):
        unreal.log(f"  [skip] {full_path} already exists")
        return unreal.load_asset(full_path)

    # UInputAction is a UDataAsset - use DataAssetFactory
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.InputAction)
    ia = asset_tools.create_asset(name, OUTPUT_DIR, unreal.InputAction, factory)

    # value_type defaults to bool (Digital) which is what we want for buttons

    # Save
    EIL.save_asset(full_path)
    unreal.log(f"  [created] {full_path}")
    return ia


# -- Main ---------------------------------------------------------------------
def main():
    unreal.log("=== Creating PSX Controller Input Assets (Xbox gamepad only) ===")

    ensure_directory(OUTPUT_DIR)

    # 1) Create all InputAction assets
    actions = {}
    for name, bit, gp in PAD_BUTTONS:
        actions[name] = create_input_action(name)

    # 2) Create InputMappingContext
    imc_name = "IMC_PSXPad"
    imc_path = f"{OUTPUT_DIR}/{imc_name}"

    if EIL.does_asset_exist(imc_path):
        unreal.log(f"  [skip] {imc_path} already exists")
        imc = unreal.load_asset(imc_path)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.InputMappingContext)
        imc = asset_tools.create_asset(imc_name, OUTPUT_DIR, unreal.InputMappingContext, factory)
        unreal.log(f"  [created] {imc_path}")

    # 3) Add Xbox controller mappings to the IMC
    for name, bit, gp_key in PAD_BUTTONS:
        ia = actions[name]
        key = unreal.Key()
        key.set_editor_property("key_name", gp_key)
        mapping = imc.map_key(ia, key)
        unreal.log(f"  {name} -> {gp_key}")

    EIL.save_asset(imc_path)

    unreal.log("=== Done! ===")
    unreal.log(f"Assets created in: {OUTPUT_DIR}")
    unreal.log(f"Keyboard is FREE for UE5 3D navigation (ZQSD + mouse)")
    unreal.log(f"Assign IMC_PSXPad to R3000EmuComponent > PadMappingContext")
    unreal.log(f"Assign each IA_Pad* to the corresponding slot")


main()
