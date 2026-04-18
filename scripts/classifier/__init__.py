"""PSX render-loop classifier orchestrator.

Automates the end-to-end workflow of classifying a PS1 game's render loops
against the 6 canonical types documented in `docs/PSX_RENDER_LOOP_PLAYBOOK.md`.
Drives the emulator MCP (r3000_emu.exe), Ghidra MCP (GhidraMCP HTTP), and an
LLM (via litellm) in sequence, then emits a `.psx3dprof` profile + JSONL log
+ Markdown report.

Entry point: `python -m scripts.classifier --help`

Design principle: the orchestrator is *glue code* only. Business intelligence
lives in the playbook (system prompt), the MCP observables, and the external
LLM.  If behaviour needs tuning, start with the playbook, not the code.
"""

__version__ = "0.1.0"
