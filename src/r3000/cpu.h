#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <array>
#include <unordered_map>
#include <vector>

#include "../log/filelog.h"
#include "../gte/gte.h"
#include "../gte/gte_backend_kind.h"
#include "../gte/gte_modern.h"
#include "../gte/gte_3d.h"
#include "../log/logger.h"
#include "bus.h"
#include "cpu_provenance_analyzer.h"

namespace r3000
{

// CPU R3000 minimal (MIPS I) pour démo éducative.
// Objectif: un coeur interprété clair, avec logs et delay slot.
class Cpu
{
  public:
    struct StepResult
    {
        enum class Kind
        {
            ok,
            halted,        // BREAK/SYSCALL (demo) ou arrêt demandé
            mem_fault,     // accès mémoire invalide / non-aligné
            illegal_instr, // opcode/funct non supporté
        };
        Kind kind{Kind::ok};
        uint32_t pc{0};
        uint32_t instr{0};
        Bus::MemFault mem_fault{};
    };

    // JIT-prep: result of running a basic block of instructions.
    // interpret_block() is purely additive — it wraps step() in a bounded
    // loop and reports why it stopped.  A future dynamic recompiler will
    // replace this function's body when a compiled block is available for
    // start_pc, but the signature and semantics stay the same.
    struct BlockResult
    {
        enum class Reason : uint8_t
        {
            MaxInsnsReached,
            BranchTaken,
            ExceptionRaised,
            StepNonOk,      // halted / mem_fault / illegal_instr
            StopOnPc,
        };
        uint32_t final_pc{0};
        uint32_t insns_executed{0};
        Reason   reason{Reason::MaxInsnsReached};
        StepResult last_step{};
    };

    Cpu(Bus& bus, rlog::Logger* logger = nullptr);

    void reset(uint32_t reset_pc);

    uint32_t pc() const
    {
        return pc_;
    }
    uint32_t reg(unsigned idx) const
    {
        return gpr_[idx & 31u];
    }
    uint32_t cop0(uint32_t idx) const
    {
        return (idx < 32u) ? cop0_[idx] : 0u;
    }

    void set_pretty(int enabled)
    {
        pretty_ = enabled ? 1 : 0;
    }

    void set_stop_on_high_ram(int enabled)
    {
        stop_on_high_ram_ = enabled ? 1 : 0;
    }

    void set_stop_on_bios_to_ram_nop(int enabled)
    {
        stop_on_bios_to_ram_nop_ = enabled ? 1 : 0;
    }

    // Debug: stop dès qu'on ENTRE dans une zone de NOPs en RAM (transition depuis une instr non-NOP).
    // C'est utile quand le BIOS part exécuter du vide vers 0x801FFxxx, mais qu'on veut voir
    // l'instruction qui a provoqué le saut (avant d'avoir 1000 lignes de 0x00000000).
    void set_stop_on_ram_nop(int enabled)
    {
        stop_on_ram_nop_ = enabled ? 1 : 0;
    }

    // Debug: stop when PC reaches an exact value (virtual address).
    void set_stop_on_pc(uint32_t pc, int enabled)
    {
        stop_on_pc_ = enabled ? 1 : 0;
        stop_pc_ = pc;
        stopped_on_pc_ = 0;
    }

    // Debug: log verbeux des accès MMIO (I/O PS1) avec nom de registre si connu.
    void set_trace_io(int enabled)
    {
        trace_io_ = enabled ? 1 : 0;
    }

    // Debug/HLE: active des trampolines sécurisés (vecteurs A0/B0/C0 et exception vector RAM).
    void set_hle_vectors(int enabled);
    // Mode intermédiaire: intercepte uniquement les sorties texte BIOS/SDK
    // (A(3Fh)=printf, B(3Dh)=putchar) tout en laissant le reste en non-HLE.
    void set_text_hle(int enabled);
    void set_hle_tcb_addr(uint32_t phys) { hle_tcb_addr_ = phys; }

    // Disable HLE pseudo-vblank when GPU generates real VBlanks.
    // Call set_use_gpu_vblank(1) when a GPU is present and handling VBlanks.
    void set_use_gpu_vblank(int enabled) { use_gpu_vblank_ = enabled ? 1 : 0; }

#ifdef R3000_DBG_LOOP_DETECTORS
    void set_loop_detectors(int enabled) { loop_detectors_ = enabled; }
#else
    void set_loop_detectors(int) {}
#endif

    // Batch bus ticking: tick every N steps instead of every step.
    // Higher values = faster but less accurate. 1 = cycle-accurate (default CLI). 32 = good for UE5.
    void set_bus_tick_batch(uint32_t n) { bus_tick_batch_ = (n < 1) ? 1 : n; }
    void set_crash_trace_steps(uint32_t n)
    {
        crash_trace_enabled_ = (n > 0) ? 1 : 0;
        crash_trace_dump_count_ = (n > kCrashTraceCapacity) ? kCrashTraceCapacity : n;
        crash_trace_pos_ = 0;
        crash_trace_count_ = 0;
    }

    // Cycle multiplier: compensate for simplified 1-cycle-per-instruction model.
    // Real MIPS R3000 averages ~1.5-2 cycles/instruction due to loads, branches, etc.
    // Default 2 gives better SPU timing (audio duration matches real hardware).
    void set_cycle_multiplier(uint32_t n) { cycle_multiplier_ = (n < 1) ? 1 : n; }
    uint32_t cycle_multiplier() const { return cycle_multiplier_; }

    // Per-instruction cycle count: returns multiplied cycles consumed by the last step().
    // Raw cost (GTE 5-44, MUL 13, DIV 36, other 1) × cycle_multiplier_.
    uint32_t last_cycles() const { return last_multiplied_cycles_; }

    // Access primary GTE backend. Returns the faithful-compatible base.
    gte::Gte& gte() { return *static_cast<gte::Gte*>(gte_backend_.get()); }
    const gte::Gte& gte() const { return *static_cast<const gte::Gte*>(gte_backend_.get()); }
    gte::BackendKind gte_backend_kind() const { return gte_backend_kind_; }
    void set_gte_backend_kind(gte::BackendKind kind);

    // Shadow GTE for differential tag encoding (3D reconstruction)
    void set_gte_shadow(gte::Gte3D* g) { gte_shadow_ = g; }
    void set_camera_analysis_enabled(int enabled) { camera_analysis_enabled_ = enabled ? 1 : 0; }
    struct CameraCandidateSnapshot
    {
        uint32_t addr{0};
        uint32_t hits{0};
        uint32_t frame_hits{0};
        uint32_t first_vblank{0};
        uint32_t last_vblank{0};
        uint32_t last_pc{0};
        uint32_t gte_reg_mask{0};
    };
    std::vector<CameraCandidateSnapshot> camera_candidates_snapshot() const;
    void restore_camera_candidates(const std::vector<CameraCandidateSnapshot>& in);
    uint64_t camera_candidates_serial() const { return camera_candidates_serial_; }

    // Debug: fichier de sortie texte (BIOS putc / syscalls "write-like" / etc).
    // Objectif: avoir un "console.log" séparé et facile à relire pendant le live.
    void set_text_out(std::FILE* f)
    {
        text_out_ = f;
    }

    // Callback for BIOS text output. Called for each character.
    // bFromPrintf=true for A(3Fh) printf, false for B(3Dh) putchar.
    using PutcharCallback = void(*)(char ch, bool bFromPrintf, void* user);
    void set_putchar_callback(PutcharCallback cb, void* user)
    {
        putchar_cb_ = cb;
        putchar_cb_user_ = user;
    }

    // Duplique le texte (BIOS putc / write-like) vers un log combiné (ex: logs/io.log).
    // On bufferise par ligne pour éviter le spam par caractère.
    void set_text_io_sink(const flog::Sink& s, const flog::Clock& c)
    {
        text_io_ = s;
        text_clock_ = c;
        text_has_clock_ = 1;
    }

    // Logs "système" pour événements CPU à fort signal (ex: boucle d'exception).
    // Objectif: pas de trace CPU/GTE complète ici, seulement quelques lignes clés.
    void set_sys_log_sinks(const flog::Sink& sys, const flog::Sink& combined, const flog::Clock& c)
    {
        sys_log_ = sys;
        sys_io_ = combined;
        sys_clock_ = c;
        sys_has_clock_ = 1;
    }

    // Compare-with-DuckStation: write parseable key=0x... blocks at debug-loop PCs.
    void set_compare_file(std::FILE* f)
    {
        compare_file_ = f;
    }

    // Petites APIs "outillage" (loader) pour initialiser un programme chargé depuis un fichier.
    // On reste minimal: pas d'OS/BIOS, donc le loader doit pouvoir positionner PC/GP/SP.
    void set_pc(uint32_t pc)
    {
        pc_ = pc;
    }

    void set_gpr(uint32_t idx, uint32_t v)
    {
        set_reg(idx, v);
    }

    uint32_t gpr(uint32_t idx) const { return (idx < 32) ? gpr_[idx] : 0; }
    uint32_t hi() const { return hi_; }
    uint32_t lo() const { return lo_; }

    void set_cop0(uint32_t idx, uint32_t v) { if (idx < 32) cop0_[idx] = v; }

    // Advanced register trace mode - logs every instruction with full register state
    // in a PC range. Use set_reg_trace() to enable and configure.
    struct RegTraceConfig
    {
        uint32_t pc_start;      // Start PC of trace range (0 = disabled)
        uint32_t pc_end;        // End PC of trace range
        uint32_t watch_value;   // Value to watch for in any register (0 = disabled)
        int enabled;            // Master enable
    };

    struct GteTraceConfig
    {
        uint32_t pc_start;      // Start PC of trace range (0 = disabled)
        uint32_t pc_end;        // End PC of trace range
        uint32_t start_frame;   // Start VBlank/frame gate (0 = immediate)
        uint32_t end_frame;     // End VBlank/frame gate (0 = unbounded)
        int enabled;            // Master enable
        int summary_dumped;     // Summary already emitted for current window
    };

    void set_reg_trace(uint32_t pc_start, uint32_t pc_end, uint32_t watch_value = 0)
    {
        reg_trace_.pc_start = pc_start;
        reg_trace_.pc_end = pc_end;
        reg_trace_.watch_value = watch_value;
        reg_trace_.enabled = (pc_start != 0 || pc_end != 0) ? 1 : 0;
    }

    void set_reg_trace_enabled(int enabled) { reg_trace_.enabled = enabled; }
    void set_gte_trace(uint32_t pc_start, uint32_t pc_end)
    {
        gte_trace_.pc_start = pc_start;
        gte_trace_.pc_end = pc_end;
        // 0:0 means "all PCs". Keep the trace enabled so callers can bound it
        // by frame without having to invent a fake PC range.
        gte_trace_.enabled = 1;
        gte_trace_.summary_dumped = 0;
        gte_trace_pc_hist_.clear();
        gte_trace_op_hist_.clear();
    }
    void set_gte_trace_frames(uint32_t start_frame, uint32_t end_frame)
    {
        gte_trace_.start_frame = start_frame;
        gte_trace_.end_frame = end_frame;
        gte_trace_.summary_dumped = 0;
        gte_trace_pc_hist_.clear();
        gte_trace_op_hist_.clear();
    }
    void set_gte_trace_enabled(int enabled)
    {
        gte_trace_.enabled = enabled;
        if (!enabled)
        {
            gte_trace_.summary_dumped = 0;
            gte_trace_pc_hist_.clear();
            gte_trace_op_hist_.clear();
        }
    }

    StepResult step();

    // JIT-prep: run step() in a bounded loop until a block boundary is hit.
    // Does not change interpreter semantics — it's a wrapper.  A compiled JIT
    // block will later replace this entry point without affecting callers.
    BlockResult interpret_block(uint32_t start_pc, uint32_t max_insns);

    // JIT-prep: monotonic tokens incremented when code-containing memory is
    // modified.  A future JIT will snapshot these at block-compile time and
    // invalidate compiled blocks on token change.  Interpreter reads them 0.
    uint64_t icache_dirty_token() const { return icache_dirty_token_; }
    uint64_t code_ram_dirty_token() const { return code_ram_dirty_token_; }

  private:
    // JIT-prep: factored IRQ-taken check.  Returns true if an external
    // interrupt was raised and step() should return early with ok/zero.
    // Called at the top of step() and (future) at block boundaries by the
    // dynamic recompiler.
    bool check_and_raise_irq();

    // COP0 minimal (suffisant pour exceptions et quelques move).
    // On reste volontairement simple pour une démo: pas de TLB, pas de timing cycle-accurate.
    // NOTE: on supporte néanmoins un minimum d'IRQ (EXC_INT) pour permettre au BIOS d'avancer.
    enum Cop0Reg : uint32_t
    {
        COP0_INDEX = 0,
        COP0_RANDOM = 1,
        COP0_ENTRYLO0 = 2,
        COP0_ENTRYLO1 = 3,
        COP0_CONTEXT = 4,
        COP0_PAGEMASK = 5,
        COP0_WIRED = 6,
        COP0_BADVADDR = 8,
        COP0_COUNT = 9,
        COP0_ENTRYHI = 10,
        COP0_COMPARE = 11,
        COP0_STATUS = 12,
        COP0_CAUSE = 13,
        COP0_EPC = 14,
        COP0_PRID = 15,
    };

    enum ExceptionCode : uint32_t
    {
        EXC_INT = 0,
        EXC_ADEL = 4, // Address error load/fetch
        EXC_ADES = 5, // Address error store
        EXC_SYS = 8,  // Syscall
        EXC_BP = 9,   // Breakpoint
        EXC_RI = 10,  // Reserved instruction
        EXC_OV = 12,  // Overflow
    };

    // Helpers de décodage (format MIPS)
    static uint32_t op(uint32_t i)
    {
        return (i >> 26) & 0x3Fu;
    }
    static uint32_t rs(uint32_t i)
    {
        return (i >> 21) & 0x1Fu;
    }
    static uint32_t rt(uint32_t i)
    {
        return (i >> 16) & 0x1Fu;
    }
    static uint32_t rd(uint32_t i)
    {
        return (i >> 11) & 0x1Fu;
    }
    static uint32_t shamt(uint32_t i)
    {
        return (i >> 6) & 0x1Fu;
    }
    static uint32_t funct(uint32_t i)
    {
        return i & 0x3Fu;
    }
    static uint16_t imm_u(uint32_t i)
    {
        return static_cast<uint16_t>(i & 0xFFFFu);
    }
    static int16_t imm_s(uint32_t i)
    {
        return static_cast<int16_t>(i & 0xFFFFu);
    }
    static uint32_t jidx(uint32_t i)
    {
        return i & 0x03FF'FFFFu;
    }

    void set_reg(uint32_t idx, uint32_t v);
    void schedule_branch(uint32_t target_after_delay_slot);


    // Traduction virtuelle->physique (simplifiée PS1): KSEG0/KSEG1 = alias sur
    // 0x0000_0000..0x1FFF_FFFF.
    uint32_t virt_to_phys(uint32_t vaddr) const;

    // Exceptions (mode minimal): set COP0(Cause/EPC/BadVAddr) et saute sur vector.
    void raise_exception(uint32_t code, uint32_t badvaddr, uint32_t pc_of_fault);

    // R3000A (MIPS I) n'a pas d'interlocks complets: les loads ont un "load delay slot".
    // Concrètement: une instruction de load met à jour rt *après* l'instruction suivante.
    struct PendingLoad
    {
        int valid;
        uint32_t reg;
        uint32_t value;
        uint32_t face_token;
        uint32_t mem_paddr;
        int from_mem;
    };

    void commit_pending_load();
    void observe_camera_mtc2(uint32_t pc, uint32_t gte_data_reg, uint32_t cpu_src_reg);
    void maybe_log_camera_candidates();

    Bus& bus_;
    rlog::Logger* logger_{nullptr};
    static constexpr uint32_t kNoFaceToken = 0xFFFFFFFFu;

    uint32_t gpr_[32]{};
    uint32_t gpr_face_token_[32]{};
    uint32_t reg_last_mem_addr_[32]{};
    uint32_t reg_last_mem_vblank_[32]{};
    uint8_t reg_last_mem_valid_[32]{};
    uint32_t hi_{0};
    uint32_t lo_{0};
    uint32_t pc_{0};
    uint32_t cop0_[32]{};

    PendingLoad pending_load_{};

    // R3000A I-cache (4KB) - utilisé comme "data store" quand COP0.Status.Isc=1 (cache isolated).
    // Pour le bring-up BIOS, l'essentiel est que les boucles d'init cache n'écrasent pas la RAM.
    std::array<uint8_t, 4u * 1024u> icache_data_{};

    // JIT-prep: monotonic invalidation tokens.
    // icache_dirty_token_ is bumped on every write into icache_data_
    // (cache_iso_write_*).  code_ram_dirty_token_ is bumped when a regular
    // store lands in the PSX 2 MB main RAM window [0, 0x00200000).  The
    // interpreter never reads them; they exist solely so a future block-based
    // recompiler can detect SMC/DMA invalidations at block entry.
    uint64_t icache_dirty_token_{0};
    uint64_t code_ram_dirty_token_{0};

    // Trace ring-buffer (dernieres instructions fetchées) pour debug BIOS.
    // Utile quand on veut comprendre un IFETCH fault sans activer --pretty.
    uint32_t recent_pc_[256]{};
    uint32_t recent_instr_[256]{};
    uint32_t recent_pos_{0};
    struct CrashTraceEntry
    {
        uint32_t pc{0};
        uint32_t instr{0};
        uint32_t hi{0};
        uint32_t lo{0};
        uint32_t status{0};
        uint32_t cause{0};
        uint32_t epc{0};
        uint32_t badvaddr{0};
        uint32_t gpr[32]{};
    };
    static constexpr uint32_t kCrashTraceCapacity = 2048;
    std::array<CrashTraceEntry, kCrashTraceCapacity> crash_trace_{};
    uint32_t crash_trace_pos_{0};
    uint32_t crash_trace_count_{0};
    uint32_t crash_trace_dump_count_{0};
    int crash_trace_enabled_{0};
    int stop_on_high_ram_{0};
    int stopped_on_high_ram_{0};
    int stop_on_bios_to_ram_nop_{0};
    int stop_on_ram_nop_{0};
    int stop_on_pc_{0};
    int stopped_on_pc_{0};
    uint32_t stop_pc_{0};

    // IFETCH ADEL repeat detector. When the BIOS exception handler can't
    // recover from a misaligned PC (the bad PC is permanent, not a transient
    // ISR glitch), the CPU loops forever between the bad PC and the handler.
    // We count consecutive ADEL faults at the same PC and halt the CPU once
    // a threshold is reached, so the worker thread / CLI can stop cleanly
    // instead of spinning and flooding logs.
    uint32_t ifetch_adel_last_pc_{0};
    uint32_t ifetch_adel_repeat_{0};
    static constexpr uint32_t kIfetchAdelHaltThreshold = 16;
    int trace_io_{0};
    uint32_t trace_io_critical_count_{0};
    int hle_vectors_{0};
    int text_hle_{0};
    std::FILE* text_out_{nullptr};
    PutcharCallback putchar_cb_{nullptr};
    void* putchar_cb_user_{nullptr};
    flog::Sink text_io_{};
    flog::Clock text_clock_{};
    int text_has_clock_{0};
    char text_line_[512]{};
    uint32_t text_pos_{0};

    flog::Sink sys_log_{};
    flog::Sink sys_io_{};
    flog::Clock sys_clock_{};
    int sys_has_clock_{0};

    uint32_t bus_tick_accum_{0};
    uint32_t bus_tick_batch_{1}; // tick bus every N steps (1 = every step, 32 = batched)
    uint32_t cycle_multiplier_{2}; // CPI multiplier (real R3000 averages ~2 CPI)
    uint32_t last_instr_cycles_{1}; // raw cycles set during instruction execution
    uint32_t last_multiplied_cycles_{2}; // last_instr_cycles_ * cycle_multiplier_ (returned by last_cycles())
    uint32_t gte_corr_diag_count_{0}; // diagnostic: limit GTE correlation debug logs
    CpuProvenanceAnalyzer provenance_analyzer_{};
    uint32_t call_ctx_hash_{0};
    uint32_t call_ctx_stack_[64]{};
    uint8_t call_ctx_sp_{0};
    struct CallCtxFaceState
    {
        uint32_t token{kNoFaceToken};
        uint64_t seq{0};
    };
    uint64_t token_seq_{0};
    std::unordered_map<uint32_t, CallCtxFaceState> call_ctx_face_tokens_{};
    int camera_analysis_enabled_{0};
    uint32_t camera_last_vblank_seen_{0};
    uint32_t camera_last_log_vblank_{0};
    uint64_t camera_candidates_serial_{0};
    struct CameraRootCandidate
    {
        uint32_t addr{0};
        uint32_t hits{0};
        uint32_t frame_hits{0};
        uint32_t first_vblank{0};
        uint32_t last_vblank{0};
        uint32_t last_pc{0};
        uint32_t gte_reg_mask{0};
    };
    std::unordered_map<uint32_t, CameraRootCandidate> camera_root_candidates_{};

    std::FILE* compare_file_{nullptr};

    uint64_t exc_vec_hits_{0};

    // Temporary exception trace: armed when PC enters game code, logs next N exceptions/RFE
    int exc_trace_armed_{0};
    int exc_trace_count_{0};
    int exc_trace_pc_log_{0};
    static constexpr int kExcTraceMax = 2000;
    uint32_t irq_take_log_count_{0};
    uint32_t irq_exc_log_count_{0};
    uint32_t irq_status_write_log_count_{0};
    uint32_t bios_handoff_log_count_{0};
    uint8_t game_boot_stage57_logged_{0};
    uint8_t game_boot_stage67_logged_{0};
    uint32_t stage67_cpu_log_count_{0};
    uint32_t stage67_entry_log_count_{0};
    uint32_t stage67_gate_log_count_{0};
    uint32_t stage67_queue_log_count_{0};
    uint32_t stage67_slot_log_count_{0};
    uint32_t stage67_write_log_count_{0};
    uint32_t stage67_pre_log_count_{0};
    uint32_t stage67_hit6742_log_count_{0};
    uint32_t stage67_hit6766_log_count_{0};
    uint32_t stage67_hit676e_log_count_{0};
    uint32_t stage67_hit691e_log_count_{0};
    uint32_t stage67_hit69f1_log_count_{0};
    uint32_t stage67_main_log_count_{0};
    uint32_t stage67_call54_log_count_{0};
    uint32_t stage67_memop_log_count_{0};
    uint32_t stage67_loop_log_count_{0};
    uint32_t stage67_caller_log_count_{0};
    uint32_t stage67_post_log_count_{0};
    uint32_t irq_rfe_log_count_{0};
    uint32_t bios_cd_pending_log_count_{0};

    // HLE BIOS vectors (bring-up): petit état pour quelques services kernel.
    // NOTE: ce n'est pas "PS1-accurate"; objectif = permettre au BIOS d'avancer,
    // tout en gardant le code lisible pour le live.
    uint32_t kalloc_ptr_{0};
    uint32_t kalloc_end_{0};
    uint32_t entryint_struct_addr_{0};
    uint32_t entryint_hook_addr_{0};
    uint32_t hle_custom_exit_handler_{0}; // B(0x19) SetCustomExitFromException
    uint32_t hle_tcb_addr_{0};            // TCB base address (physical)
    uint32_t hle_event_table_addr_{0};    // Event table base in RAM (physical)

    struct HleEvent
    {
        uint32_t cls;
        uint32_t spec;
        uint32_t mode;
        uint32_t func;
        uint32_t status;
    };

    HleEvent hle_events_[32]{};

    // Deliver an event (set matching events from busy to ready).
    // Same logic as B(07h) DeliverEvent, but callable from the HLE exception handler.
    // Returns number of events matched.
    int hle_deliver_event(uint32_t cls, uint32_t spec)
    {
        int found = 0;
        for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
        {
            HleEvent& e = hle_events_[i];
            if ((e.status & 0x2000u) && e.cls == cls && e.spec == spec)
            {
                e.status &= ~0x2000u;
                e.status |= 0x4000u;
                found++;
            }
        }
        return found;
    }

    uint32_t hle_vblank_div_{0};
    uint32_t hle_vblank_counter_{0}; // Software VBlank frame counter (root counter 3)
    int hle_pseudo_vblank_{0};
    int use_gpu_vblank_{0}; // When 1, GPU generates VBlanks - disable HLE pseudo-vblank
    uint32_t hle_rand_seed_{0};

    // HLE BIOS File I/O (cdrom:) - minimal pour permettre le boot CD.
    struct HleFile
    {
        int used;
        uint32_t lba;   // LBA ISO9660 (secteurs 2048)
        uint32_t size;  // taille fichier en bytes
        uint32_t pos;   // position courante en bytes
    };

    HleFile hle_files_[16]{};
    uint32_t hle_last_error_{0};
    uint64_t hle_wait_event_calls_{0};
    uint64_t hle_mark_ready_calls_{0};
#ifdef R3000_DBG_LOOP_DETECTORS
    int loop_detectors_{1};
    uint32_t dbg_loop_dumped_{0};
    uint32_t dbg_loop_patched_{0};
    uint32_t dbg_ef30_dumped_{0};
    uint32_t dbg_de24_dumped_{0};
    uint32_t dbg_e520_dumped_{0};
    uint32_t dbg_6797c_dumped_{0};
    uint32_t dbg_67938_dumped_{0};
#endif
    uint32_t spin_pc_{0};
    uint32_t spin_count_{0};
    uint32_t pc_sample_counter_{0};
    uint32_t irq_loop_istat_{0};  // IRQ loop detector: last pending I_STAT
    uint32_t irq_loop_count_{0};  // IRQ loop detector: consecutive entries
    // NOTE: pas de "skip loop" ici: on préfère corriger l'émulation plutôt que patcher le flow du BIOS.

    // COP2 = GTE (PS1). Séparé du CPU pour garder le code propre.
    std::unique_ptr<gte::IGte> gte_backend_{};
    gte::BackendKind gte_backend_kind_{gte::BackendKind::faithful};
    gte::Gte3D* gte_shadow_{nullptr}; // Shadow GTE for 3D tag encoding

    // MIPS: branchements/jumps ont un delay slot.
    // On stocke le PC à appliquer *après* l'instruction suivante.
    bool branch_pending_{false};
    uint32_t branch_target_{0};
    uint32_t branch_delay_slots_{0}; // 1 = un delay slot restant
    bool branch_just_scheduled_{false};

    int pretty_{0};
    int ri_trace_count_{0};
    int aerr_trace_count_{0};

    // Advanced register trace
    RegTraceConfig reg_trace_{};
    GteTraceConfig gte_trace_{};
    std::unordered_map<uint32_t, uint32_t> gte_trace_pc_hist_{};
    std::unordered_map<uint32_t, uint32_t> gte_trace_op_hist_{};
    void record_crash_trace(uint32_t instr);
    void dump_crash_trace(const char* reason) const;
};

} // namespace r3000
