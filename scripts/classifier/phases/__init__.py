"""Classifier pipeline phases.

Each phase is a single module exporting a top-level ``run(ctx)`` function
that takes the orchestrator context and mutates it in place, advancing the
pipeline state by one step.

Phases:
    p1_boot          : launch r3000_emu.exe, wait until game reaches RAM-high PC
    p2_gte_discovery : list GTE-producing functions (Ghidra + runtime cross-ref)
    p3_loop_discovery: walk xrefs to find outer display loops
    p3b_gte_trap     : widen GTE trace window + inject pad inputs to trigger 3D
    p4_classify      : per-loop LLM classification against the playbook
    p5_profile       : serialize accepted classifications into .psx3dprof
"""
