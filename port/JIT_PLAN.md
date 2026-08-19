# STAGE 4 — THE JIT (aarch64) — THE PLAN

Companion to `ANDROID_PLAN.md` (same structure: measured facts, verdict, architecture,
staged plan with hard gates, risks). Read `PROJECT_STATUS.md` sessions 24–30 first —
this document assumes the interpreter (S1), the dispatch boundary (S2/26), the det
batteries (S3), the ARM cross (S5/27), the on-device port (S6/28-29) and the FAST x87
unit are all landed and proven, because the JIT is built ON those proofs, not beside
them.

**Scope rule up front: the JIT is aarch64-only.** No file in `port/src/engine/jit/`
is ever listed in the Windows CMakeLists. Windows x86 keeps the interpreter +
form cache byte-for-byte (`eng_test` fingerprint 49,935,104 untouched by
construction, same rule as every Stage-5 landing). The interpreter remains complete
and shipped on ARM too — it is the reference, the fallback for anything the JIT
declines, and the A/B leg of every gate.

---

## 0. MEASURED FACTS (session-30 profile — det-neutral, cross-validated)

| fact | value | consequence |
|---|---|---|
| in-match frame cost on device | **~29 ms/f** | need ~12–13 ms/f for 60 fps |
| split | **int core 57.4% (16.6 ms) / x87-FAST 29.4% (8.5 ms) / hook bodies 13.1% (3.8 ms)** | the JIT attacks the first two; hooks are native already |
| decode share | **~0** (cache hit 88.6%, NOCACHE A/B ~0% delta) | ⚠ **decode is NOT the cost** — per-op dispatch + EA + flags + memory helpers are. A fancier *cache* wins nothing; only removing the per-op interpreter tax wins |
| insns/frame in-match | 898k–1,538k (~1.2M typ.) | at 60 fps ≈ 55–92M insns/s effective |
| x87 share of insns | ~25% | x87 must be inlined eventually, not just called faster |
| dispatch-boundary crossings | **1,675/frame** | escape/dispatch round-trips are ~0.1–0.2 ms/f total — the C boundary can STAY in C |
| GCALLs (host→guest) | 13/frame | nested Run re-entry is rare; must be *correct*, not fast |
| FAST-x87 fallback rate | **0.004%** | the FAST unit's committed paths cover in-match reality; inlining them inlines ~everything |
| basic-block shape (MSVC-era x86) | ~5–7 insns/block ⇒ ~200k block transitions/frame | ⚠ a naive block-dispatch loop costs 3–5 ms/f BY ITSELF (200k × 15–25 ns) — **block linking is load-bearing, not a luxury** |

**The budget arithmetic the plan must satisfy:**
int 16.6/3.5 ≈ 4.8 ms + x87 8.5/2 ≈ 4.3 ms + hooks 3.8 ms ≈ **12.9 ms/f**.
Sanity: ~900k int insns in 4.8 ms = 188M/s ≈ 16 host cycles per guest instruction on
a ~3 GHz core — comfortably realistic for pinned-register emitted code, far out of
reach for any interpreter loop (each interpreted op pays lookup + gen check + dispatch
+ helper calls before doing any work).

---

## 1. THE COMPILATION UNIT — blocks grown from the proven decoder

**Unit: the straight-line basic block, discovered lazily at execution time, decoded by
the SAME classifier family the form cache uses.** Not traces, not superblocks — the
det-oracle discipline of this project wants the smallest unit whose entry/exit state
is architecturally complete, and MSVC-era code has no loop shapes that reward tracing
enough to pay its verification cost. (Block *linking* in M2b/M4 recovers most of what
traces would buy, without speculative tails.)

### 1.1 Discovery and decoding

- A block starts at any EIP the engine actually reaches inside the exec window
  (`0x11000..0x10000000` — the WHOLE guest window: Xbox has no NX, the game runs
  heap-resident code at round-over and the tramp pad at `0x03F00000`, all of it is
  eligible, exactly as today).
- Decode forward with a block-level extension of `TryCacheSite`'s classifier
  (`x86_interp.cpp`): same prefix walk, same `ParseHotModRM`, producing a linear
  **form trace** (an array of `HotDec`-shaped records). The decoder MUST remain the
  one already differentially proven — the JIT adds no second opinion about x86
  semantics, only about how fast the proven semantics run.
- A block **ends** at:
  1. any control transfer — `jcc`, `jmp`, `call`, `ret/retn`, indirect `call/jmp r/m`
     (`FF /2 /4`);
  2. the first instruction the classifier declines (66/F2/F3/F0 prefixes, string
     ops, div/idiv, cmpxchg, any BadDecode-risk form) — the block exits with `eip`
     at that instruction and the outer loop runs ONE instruction through the
     untouched slow switch, then re-enters block dispatch. Rare forms cost one
     interpreter step; nothing is re-implemented;
  3. a 64-instruction cap, or the 4th distinct page touched by code bytes (keeps the
     per-block generation list bounded; an instruction still never spans a page —
     the classifier's existing refusal carries over);
  4. the exec-window edge.
- Direct `call rel32` ends the block (push return, transfer); the return address
  starts a fresh block. Direct `jcc` records BOTH successor EIPs for linking.

### 1.2 Entry/exit rules — where the engine's invariants live

The JIT slots into `Run()`'s existing loop as a tier ABOVE the form cache:

```
stopEip check → EIP ring store → exec-range/escape check → maxSteps
   → [JIT] block lookup+dispatch (aarch64, TJ_ENG_JIT=1)
   → form cache → slow switch
```

- **Sentinels / nested Run.** `stopEip` is only ever reached via a `ret` or an
  explicit transfer, and every sentinel is OUTSIDE the exec window by construction
  (`0xF00D0001/2` on ARM — fixed constants, engine_mode.cpp). A compiled block
  therefore cannot "fall through" onto a sentinel; any transfer to an out-of-window
  address exits emitted code, and the outer loop's stop check runs before the next
  dispatch. GCALL's nested `Run()` re-enters the same loop and dispatches into the
  same block cache — the block cache is read-mostly global state and `Run` stays
  reentrant exactly as today (compilation itself only ever happens on the engine
  thread; the guest is single-threaded).
- **Host-call escapes / dispatch keys.** On ARM every guest→host transition is a
  32-bit KEY (synthetic handle `0xE0000000+n·16`, or a patched rel32 resolving to
  one) — always OUTSIDE the exec window, always reached by a control transfer,
  **never mid-instruction**. So "escapes reachable mid-block" reduces to: every
  control transfer whose target is out-of-window (statically, for direct patched
  targets; dynamically, for indirect ones through thunk slots/vtables) is compiled
  as *store outgoing state, set `s.eip = target`, return to the dispatcher* — and
  the C `HostEscape` path (jmp-hooks first, then `DispatchTryInvoke`, then the
  ARM FATAL for unregistered targets) runs UNCHANGED. At 1,675 crossings/frame the
  C boundary costs ~0.1–0.2 ms/f: **zero dispatch knowledge is burned into emitted
  code**, which also keeps every A/B switch (`TJ_ENG_NODISPATCH`, `TJ_ENG_GATECALL`,
  `TJ_ENG_RAWHOOKS`) meaningful under the JIT.
- **Jmp-hooks** (`Hk_RoundEnd` at `0x172EC` etc.): their entry keys are host-range
  values on ARM, so the same rule covers them; the resume VA (`0x172F3`) simply
  starts a block. No [esp] displacement anywhere — that whole crash class stays
  designed out.
- **maxSteps.** Kept at block granularity: a per-block `subs` on a budget cell in
  CpuState; exhaustion exits with MaxSteps at the block entry EIP (± ≤64 insns from
  the interpreter's exact point). The device runs `~0ull`; eng_sweep — the one
  consumer that needs exact budgets — is Windows-x86-only and never sees the JIT.
  `g_instrTotal` accrues block lengths (det-neutral, keeps the `[eng-mode] STOPPED`
  line and prof2 honest).
- **EIP ring.** One store per BLOCK entry (prologue), not per instruction — the
  interpreter path keeps per-insn stores. Crash forensics get block granularity
  plus the §5 side table for the faulting instruction; prof2's sampled attribution
  becomes block-entry-weighted (acceptable: it is temporary instrumentation).

### 1.3 Self-modifying code — the same generation scheme, extended honestly

The proven machinery stays authoritative: guest stores run `SmcBar` (page bitmap of
"holds cached sites" → generation bump), host-side patchers call
`EngineModeInvalidateCode` (the ONLY two runtime patchers: meat_rush's `SetBars`,
dsound's `RestoreGameFn`), and `EngineSetExecRange` resets everything (the body
re-asserts it after installs on both platforms — the JIT rides the same reset:
**full block-cache + code-cache flush**, links severed).

JIT-specific extensions:

1. **Per-block generation list.** Each block records `(page, gen)` for every page
   its code bytes touch (≤4 by rule 1.1.3). The **block prologue re-checks the
   list** — this is the same check the form cache does per instruction, hoisted to
   block entry. Stale → exit to dispatcher → re-decode/re-compile. Because LINKS
   jump to a successor's *prologue* (never past it), a stale block is unreachable
   through chains too — self-healing, no back-reference bookkeeping needed for
   correctness (an unlink pass on gen bump is a later optimization, not a
   correctness requirement).
2. **Same-page store side-exit.** The interpreter is precise to the instruction: a
   store that dirties a code page invalidates before the NEXT instruction decodes.
   A compiled block that stores into its own page must not keep executing stale
   translation. The emitted store barrier already branches to a slow path when the
   page bitmap bit is set (rare by construction — only pages holding cached code);
   that slow path additionally checks "did I bump a page on MY OWN block's list?"
   and if so **side-exits with `eip` = next instruction** after completing the
   store. Store-then-execute-modified-successor within one straight line is thereby
   exact, at zero cost on the fast path.
3. Native byte-patch toggles remain invisible to store barriers exactly as today —
   but both known patchers already call the invalidate seam, and the coverage rule
   stays: any NEW runtime patcher must call `EngineInvalidateCode` (engine.h
   documents it; the JIT changes nothing about the contract).

---

## 2. CODEGEN STRATEGY — options, verdict, and the three hard sub-problems

### 2.1 The options, honestly

| option | expected int-core gain | risk / cost | verdict |
|---|---|---|---|
| **(a) computed-goto threaded interpreter** over pre-decoded block traces (pure C, clang labels-as-values) | 1.5–2x (removes per-insn lookup/gen-check/switch; keeps helper calls) | lowest; portable; also speeds the qemu det legs | **BUILD FIRST (M1)** — it constructs the entire block layer (discovery, cache, SMC lists, dispatch loop) that the emitter then plugs into, and it is the permanent fallback if a device refuses executable memory |
| **(b) template/copy-patch JIT**: per-form aarch64 stencils, guest regs in MEMORY (CpuState) | 2–3x | medium; no register state to reason about at exits | rejected as the endgame — memory-resident registers pay 2 loads + 1 store per op and cap below the 3–4x target; acceptable only as an emitter stepping stone |
| **(c) direct emission with FIXED register pinning** — the 8 guest GPRs live in w19–w26 for the whole JIT residency, no allocator | 3–5x | the real thing; state mapping is STATIC so exits/faults stay tractable | **THE ENDGAME (M2)** — fixed pinning gives register-allocation's speed without an allocator's search space or its per-block state maps |
| full linear-scan RA over blocks/traces | 4–6x | two implementations of truth about liveness; per-exit location maps; the verification burden the plan exists to avoid | rejected — the target is 3–4x, (c) reaches it without this |

**Verdict: M1 = (a) as the block substrate; M2 = (c) plugged into M1's substrate,
with direct-branch linking in the same milestone (M2b) because §0's arithmetic says
an unlinked block dispatcher eats 3–5 ms/f of the win.** (b) exists only as the
natural first day of M2 (emit each form naively, then pin).

### 2.2 The fixed pinning map (AAPCS64-clean)

| host reg | role |
|---|---|
| w19–w26 | guest EAX ECX EDX EBX ESP EBP ESI EDI (Reg-enum order; 32-bit views — writes zero-extend, which IS the gptr rule) |
| x27 | `CpuState*` (spill target, helper-call base) |
| w28 | materialized EFLAGS (the F_* subset), see §2.3 |
| x0–x17, d0–d7 | scratch (caller-saved; dead across helper calls by ABI) |
| d8–d15 | M3b: block-local ST-slot cache (callee-saved → survives helper calls for free) |

One `enter_jit` thunk per dispatcher entry saves x19–x28 (+d8–d15 when M3b lands),
loads guest regs from CpuState, runs the block CHAIN, stores back on exit. Helper
calls from emitted code (store-barrier slow path, X87Exec fallback, div fault path)
preserve the pinned set automatically by ABI — **CpuState is authoritative outside
emitted code, pinned registers inside; the sync points are the enter/exit thunks
plus the enumerated helpers that read CpuState fields** (only `fnstsw ax` touches a
GPR through X87Exec — its stencil syncs EAX around the call; string/div helpers take
explicit args, never CpuState).

A fixed map has one more payoff this project specifically values: **the SIGSEGV
forensics can recover every guest register from the signal ucontext directly**
(mcontext x19–x26 ARE the guest registers, by construction) — `EngineModeCrashDump`
stays fully informative inside JIT code.

### 2.3 EFLAGS

- **Fused compare+branch first.** MSVC emits `cmp/test` + `jcc` adjacent in the
  overwhelming majority; the block compiler pattern-matches producer/consumer pairs
  and emits `subs/ands` + `b.cond` directly on NZCV — zero flag materialization on
  the hot path. This is where most of the flag win is, and it is exact.
- **Architectural bits eagerly, cheap bits lazily.** For flag-producing ops whose
  consumer is NOT fused: materialize CF/ZF/SF/OF into w28 with cset chains off NZCV
  (x86 CF = NOT ARM C after `subs` — the inversion is per-form knowledge in the
  stencil, encoded once). **PF and AF are deferred**: a 2-register lazy record
  (result byte, a^b^res nibble) in CpuState scratch, materialized ONLY at the rare
  consumers (`pushfd/lahf/setcc jp/jpe`, popfd merge) using the same `g_parity`
  table. The lazy record computes the identical bits the eager interpreter
  computes — deferral, not approximation, so det identity is preserved bit-for-bit
  at every consumer.
- **At every block exit, escape, side-exit and helper boundary, `s.eflags` holds
  the full materialized value** (w28 merged with lazily-derived PF/AF) — the
  interpreter, the dispatcher, hooks and pushfd all see exactly today's state.
  Within a block, dead-flag elimination (skip w28 updates when the next in-block
  producer overwrites before any exit/consumer) is an M4 refinement gated on the
  same batteries.

### 2.4 Guest memory access

Guest VA == host VA below 4 GB, so an access is: compute the EA in a 32-bit
register (base+idx·scale+disp folds into 1–2 ALU ops; `HF_SEGFS` adds the pinned
fs base), then `ldr/str Wt, [Xea]` with the EA register's 64-bit view — the
zero-extension IS the `mov wN, wM` semantics, no masking, no translation, no bounds
check. Rules carried over verbatim:

- **4-byte-slot rule**: emitted code writes guest memory only in the guest's own
  operand sizes (1/2/4, x87 8/10). Host-pointer-sized stores into guest memory are
  structurally impossible — emitted code never holds a host pointer in a
  guest-visible register (block-chain branch targets are host-PC-relative inside
  the code cache, invisible to guest state).
- **gptr rule**: nothing in the JIT manufactures guest addresses from host ones;
  all guest addresses arrive from guest state. The `>4GB` FATAL tripwires at the
  dispatch boundary stay armed and unchanged.
- **Audit ranges**: if `EngineSetAuditRanges` is armed (`g_auditN > 0`) the JIT
  refuses to engage (blocks are not entered) — the audit is a sweep-harness
  affordance, Windows-only in practice; belt and braces on ARM.
- **Store barrier**: every emitted store carries the inline SMC check
  (window-offset compare + bitmap bit test ≈ 4 insns, fast path falls through;
  slow path = C helper: gen bump + own-block side-exit test per §1.3.2).
  mprotect-based SMC detection was considered and REJECTED: the guest window
  deliberately mixes code and data on the same pages (heap code, stack, tramp pad)
  — write-protecting them would fault on ordinary data stores constantly.

---

## 3. X87 INTEGRATION — inlining the FAST unit's committed paths

The measured shape: 25% of insns are x87; the FAST unit already commits 99.996% of
in-match ops on its host-double paths; its cost is per-op OVERHEAD, not arithmetic —
image load/validate (`CW` gates re-checked every op), `ReadDouble`/`WriteDouble`
80↔64 conversion, the logical-slot rotation (`PushRaw`/`Pop` move 70–80 bytes per
push/pop!), TwoSum/fma flag derivation, `Commit`. Two designs, both analyzed:

### 3.1 (M3a) Inline-on-image — compile the FAST paths, keep the image resident

Emit the FAST unit's per-form fast path directly into the block, still operating on
the guest-visible 108-byte FNSAVE image:

- **Hoist the invariant gates to block entry**: one check of `CW == 0x027F-normal,
  all-masked, RC=RN, PC=53` + `no pending unmasked SW` covers every x87 op in the
  block (the game holds CW=027F; `fldcw` inside a block ends the x87 assumption —
  re-check after it). This alone deletes the per-op re-validation.
- Slot addressing, tag updates, `ReadDouble` validation (~8 insns of bit tests),
  `WriteDouble`, TwoSum/fma PE/C1 — all emitted inline; `fnstsw ax`, `fxch`,
  `ffree`, compares become tiny fixed sequences.
- **Any gate failure or non-committed class → the op falls back EXACTLY as today**:
  spill nothing (the image IS the state), call `X87Exec` (which runs
  FAST-then-EXACT on the untouched image), side-exit on its false return (#MF /
  BadDecode) with precise `eip`. The fallback contract is untouched; the 0.004%
  keeps paying.
- Honest ceiling: the push/pop rotations and 80↔64 conversions remain. Expected
  x87-phase gain ~1.5–2x. **Risk: LOW** — state is guest-visible at every
  instruction boundary, exactly like today.

### 3.2 (M3b) Block-local ST register cache — doubles in d8–d15, static TOP

Within one block, x87 stack discipline is static (MSVC balances pushes/pops in
straight-line code): the compiler tracks TOP symbolically, maps logical ST slots to
d8–d15, and:

- loads a slot from the image through `ReadDouble` ONCE on first touch (refusal →
  compile that op as an M3a image op or fall back — mixed blocks are legal because
  the flush rule below runs first);
- runs arithmetic as bare `fadd/fmul/fdiv/fsqrt d,d,d` plus the fma/TwoSum residual
  ops for PE/C1 (sticky PE accumulates in a scratch reg, OR-ed into SW at flush;
  C1 is last-op-wins, materialized at flush);
- **eliminates the rotations entirely**: push/pop become a compile-time remap; the
  NET slot movement is applied to the image once, at flush.
- **The flush rule (the correctness heart): the FNSAVE image is re-materialized —
  slots via `WriteDouble`, SW (TOP/PE/C1/condition codes), TW — at EVERY point
  another observer can see it**: block exit (incl. chained exit in v1 — cache
  lifetime is one block), any escape/dispatch/GCALL, any fallback call into
  `X87Exec`, any image-reading op compiled in-block (`fnstsw/fnstenv/fldcw/
  fnsave`), and the MaxSteps/side-exit paths. These are all statically known
  compile-time points — the flush is emitted, not discovered.
- **Faults**: a wild `fld m32` inside a cached region faults with the image stale.
  On ARM a guest memory fault is TERMINAL by design (host_compat compiles
  `__try` away; the engine reports and dumps — there is no guest-visible
  resumption). So fault-point exactness is a FORENSICS problem, not an
  architectural one: the signal handler reads the pinned d-regs + the §5 side
  table's per-PC flush recipe and reconstructs a best-effort image for the dump,
  clearly labeled. The det gates never see this path (a det leg that faults has
  already failed louder).

**What must be architecturally exact at every possible exit — the checklist both
designs are audited against:** CW (hashed in the det log EVERY frame — `cw=027F`
per line), SW with TOP/C0–C3/C1/sticky flags incl. PE, TW per physical register,
all eight 10-byte slots in logical order, and `s.r[EAX]` after `fnstsw ax`.
FIP/FCS/FOO/FOS mirror the FAST unit (untouched). The EXACT unit stays the sole
owner of unmasked-exception semantics, NaN/denormal/unnormal classes, and
transcendental kernels — the JIT inlines only what `x87_fast.cpp` COMMITS, and the
det battery that proved FAST≡EXACT over 149,401 frames is re-run identically over
the inlined form (§5).

**Order: M3a first, M3b only if M3a's measurement says the x87 phase is still
>4.5 ms/f.** M3a's fallback plumbing (side-exits, EA passing, EAX sync) is reused
by M3b unchanged; M3b adds only the cache map + flush emission.

---

## 4. DETERMINISM + VERIFICATION PLAN

The controls (all environment, all default-off, mirroring the TJ_ENG_FAST rollout):

| switch | meaning |
|---|---|
| `TJ_ENG_JIT=1` | enable the JIT tier (aarch64 builds only; ignored loudly elsewhere). Default OFF until M2's device gates pass; then the phone's `game_main` sets it no-overwrite, headless det legs stay explicit |
| `TJ_ENG_JIT_MAXBLOCKS=N` | compile-and-execute only the first N blocks by COMPILE ORDINAL; every later block runs interpreted. Compile order is deterministic (single engine thread, deterministic sim → deterministic trigger order), so **N bisects a det divergence to ONE block** — the classic tool, rebuilt |
| `TJ_ENG_JIT_DUMP=va` | print a block's form trace + emitted aarch64 + gen list at compile time |
| `TJ_ENG_JIT_STATS=1` | per-run: blocks compiled, code bytes, chains taken, side-exits by cause, x87 inline/fallback counts |

**The gate per milestone (unchanged from the project's standing method):**

1. **qemu same-binary A/B**: `tj_headless_static` under qemu-aarch64, full scripted
   MEAT battery, `TJ_ENG_JIT=1` vs unset — TJ_DETLOG **byte-identical**, TJ_ITEMLOG
   identical. This is the S25 rule: one binary, two mechanisms, zero diffs.
2. **qemu vs the Windows EXACT reference (`exS27.det`)**: the standing det leg,
   now with the JIT on — byte-identical over the full frame span. Run BOTH FPU
   configs: the EXACT leg (JIT int-core only — x87 inlining is FAST-only by
   construction and disengages under EXACT mode) and the FAST leg (which the
   149k-frame FAST≡EXACT battery already anchors).
3. **Device leg (`arm_run.ps1`)**: same comparison on the OnePlus. ⚠ The qemu leg
   CANNOT prove two device-only things: **icache coherence** (qemu-user's
   translation cache forgives a missing `__builtin___clear_cache`; real cores do
   not) and **>4GB/W^X behavior** (qemu's address-space and SELinux shape differ).
   Device runs are mandatory per milestone, not optional — the same lesson S5c
   taught about gptr bugs.
4. **Windows unaffected**: diff_test 141/141 + eng_test 141/141 with the identical
   49,935,104-insn fingerprint, per landing — expected to be trivially green since
   no Windows target compiles JIT sources, and run anyway (verify by reproduction,
   not by construction claims).
5. **In-match measurement**: `[d3d8] frame N: ms/f` via logcat + `TJ_ENG_JIT_STATS`,
   published per milestone (the S4c analogue).

**Debugging a divergence** (the drill, in order): reproduce under qemu with the
same binary; bisect `TJ_ENG_JIT_MAXBLOCKS` to the guilty ordinal; `TJ_ENG_JIT_DUMP`
its VA; compare its form trace against the slow decoder (the compiler CROSS-CHECKS
every block against per-insn slow decode at compile time already — a trace mismatch
is a compile-time FATAL, so surviving divergences are codegen, not decode); then
single-block A/B: run with only that block compiled vs none. Fault forensics:
`EngineModeCrashDump` + the pinned-reg recovery (§2.2) + the per-block side table
(host-PC → guest EIP map + M3b flush recipe).

---

## 5. STAGED MILESTONES — each independently det-gated and shippable

### M1 — Block substrate + threaded execution (pure C)
- **Scope**: block discovery/termination rules (§1.1), the block cache (open-addressed
  on entry EIP, above-4GB side arena), per-block gen lists + prologue checks, the
  store-barrier own-block side-exit, the Run-loop tier, and a computed-goto executor
  over the pre-decoded form trace whose label bodies call the SAME
  `AluOp/Flags/ld/st/Push/Cond/X87Exec` helpers (semantics shared, never
  re-implemented — the form-cache rule at block scale).
- **Files**: `port/src/engine/jit/jit_blocks.cpp` (+ `jit.h`), edits in
  `x86_interp.cpp` (tier hook, reset in `EngineSetExecRange`), `engine.h`
  (EngineSetJit + stats), env plumbing in `engine_mode.cpp`/`headless_main.cpp`,
  `port/android/CMakeLists.txt` only.
- **Risks**: block-boundary semantics (stop/escape/maxSteps ordering) — covered by
  the A/B gate; none of it is speculative.
- **Exit criteria**: gates §4.1–4 green; device in-match **~20–24 ms/f** (int phase
  ~1.6–2x); JIT stats show >95% of in-match insns executing from blocks.

### M2 — aarch64 emission, pinned registers (M2a) + direct-branch linking (M2b)
- **Scope**: hand-rolled uint32 emitter (no external assembler — self-contained,
  ~1,200 lines), stencils for the M1 form set with the §2.2 pinning and §2.3 flags;
  x87 ops emitted as `X87Exec` helper calls (EA in a register, EAX synced around
  `fnstsw ax`) so blocks stay long; unclassified forms end blocks (one-insn
  interpreter fallback). M2b: lazy direct-link patching — block exits for `jmp`/
  `jcc`/`call` targets in-window patch themselves to the successor's PROLOGUE on
  first take; `ret`/indirect exits return to the dispatcher (M4 upgrades them).
  Code cache: 32 MB cap above 4 GB, RWX-try/RW→RX-fallback, `clear_cache` after
  every write, flush-all on full or on exec-range reset.
- **Risks**: flag-mapping bugs (CF inversion, PF/AF deferral) — the bisect switch +
  batteries exist for exactly this; W^X variance (§6); chain-vs-invalidation races
  are excluded by prologue-checks-only linking.
- **Exit criteria**: gates green; device in-match **~14–17 ms/f** (int phase ≥3x);
  zero unregistered-target regressions (dispatch boundary untouched by
  construction); Windows fingerprint identical.

### M3 — x87 inlining (M3a image-resident; M3b register-cached, measure-gated)
- **Scope**: §3. M3a: block-entry CW/SW gate hoist + inline committed FAST paths on
  the image, per-op fallback preserved. M3b (only if x87 phase still >4.5 ms/f):
  d8–d15 slot cache, static TOP, flush points per §3.2, side-table flush recipes.
- **Files**: `port/src/engine/jit/jit_x87.cpp`; x87_fast.cpp untouched (it remains
  the semantic reference the inlined code is generated FROM and A/B'd against).
- **Risks**: the highest of the plan — guest-visible FPU state at boundaries.
  Mitigations: the §3 exactness checklist audited per stencil; the FAST≡EXACT
  149k-frame battery re-run with inlining on; M3b behind its own sub-switch
  (`TJ_ENG_JIT_X87=0/1/2` = off/image/cached) for bisection.
- **Exit criteria**: gates green under BOTH FPU legs; x87 phase ≥2x (≤4.3 ms/f);
  device in-match **~12–13 ms/f = 60 fps sustained**; FAST fallback rate unchanged
  (~0.004%) proving the inlined gates mirror the unit's.

### M4 — chain completion + polish (only as measurements demand)
- **Scope**: indirect-branch inline lookup (hash probe emitted at `ret`/`call r/m`
  exits, miss → dispatcher), optional return-target cache; dead-flag elimination
  within blocks; eager unlink on invalidation if prologue checks ever show up in a
  profile; thermal headroom soak (the 20-minute session, ANDROID_PLAN risk #8).
- **Exit criteria**: gates green; published ms/f with headroom (target ~10–12 ms/f
  sustained under throttle); a soak-length det leg on device.

Every milestone lands default-OFF, is proven, and only then flips the phone default
(`game_main` setenv no-overwrite) — the exact TJ_ENG_FAST rollout shape. If any
milestone's gate will not go green, the previous milestone is the shipping state
and remains playable.

---

## 6. RISKS

| # | risk | mitigation |
|---|---|---|
| 1 | **W^X / SELinux on some device or future policy refuses executable anon memory.** The game runs as the ART-free exec'd subprocess in the app's untrusted_app domain, which permits `execmem` (every JS engine relies on it) — RWX anon mmap is expected to work, and eng-space proof is one `mmap(PROT_RWX)` probe at JIT init. | CodeCache dual path: try RWX; on EPERM fall back to RW-emit → `mprotect(RX)` flip per chunk; on total refusal, print loudly and stay on M1's threaded interpreter (playable ~18–22 fps, not a brick). `TJ_ENG_JIT=0` always available. |
| 2 | **qemu-vs-device divergence for JIT pages**: qemu-user forgives missing icache maintenance and models neither real W^X nor >4GB layout faithfully (the S5c gptr lesson). | `__builtin___clear_cache` after EVERY code write on BOTH paths (also correct under qemu); device leg mandatory per milestone (§4.3); the code cache allocates via plain high mmap — NEVER through host_compat's below-4GB prober (see #4). |
| 3 | **icache/dcache coherence on big.LITTLE** (a block written on one core, executed after migration). | clear_cache does the full IC IVAU/DC CVAU dance per ARM ARM; compilation and execution are the same (engine) thread; no cross-thread codegen exists. |
| 4 | **The code cache eats guest address space.** Everything allocated through the POSIX host_compat layer is deliberately below-4GB (guest-visible by design); 32 MB of JIT pages there would collide with the pool/arena reservations that already fought ART for the low 256 MB. | The JIT allocates with plain `mmap(NULL, …)` and FATALs if the result is below 4 GB. Budget: 32 MB cap + flush-all policy (block re-compile is µs-cheap; a flush is a hiccup, not a leak). Metadata arena same rule. |
| 5 | **Block-cache invalidation misses a patcher** (a future runtime byte-patch that forgets `EngineInvalidateCode`). | Same exposure the form cache has TODAY, same contract (engine.h); the JIT adds the own-store side-exit (§1.3.2) so guest-side SMC is airtight; host-side coverage is enforced by the standing rule + the det batteries that catch a stale block as a divergence, not a mystery. |
| 6 | **16 MB engine-thread stack interaction.** | Emitted code uses small leaf frames and helper calls only; gate shadows carve from the GUEST stack, unchanged; GCALL nesting depth is unchanged (each nested Run adds one dispatcher frame as today). No new recursion. Measured stack high-water added to `TJ_ENG_JIT_STATS` to verify, not assume. |
| 7 | **Sentinel / nested-Run semantics broken by chaining** (a chained loop that never re-checks stop). | Sentinels are out-of-window constants → always exit emitted code (§1.2); chains link only in-window direct targets; `ret` exits to the dispatcher until M4, and M4's inline lookup structurally cannot match an out-of-window key. The A/B batteries run LAN `-EngineJoin` style legs where sentinel bugs surface as instant desyncs. |
| 8 | **EFLAGS deferral leaks** (PF/AF wrong at a rare consumer). | Deferral computes the same bits later, from the same recorded operands; consumers enumerated (pushfd/lahf/setcc-P/popfd); the sweep-class coverage exists on Windows for the interpreter and the JIT's A/B identity gate covers the rest. Suspicion tool: `TJ_ENG_JIT_MAXBLOCKS` bisect. |
| 9 | **x87 inline state visible half-updated at an unconsidered boundary.** | The §3 checklist + the flush-point enumeration are the design; every boundary class already exists in the interpreter (escape/GCALL/fallback/image-op/fault) and each has a named rule; M3 ships behind its own tri-state switch; fault paths are terminal-forensic on ARM by design, removing the resumption case entirely. |
| 10 | **BTI/PAC on newer Android**: pages mapped with PROT_BTI would require landing pads; PAC only affects signed returns. | Our own mmap omits PROT_BTI (no enforcement on those pages); emit `bti c` at every block prologue anyway (1 insn, future-proof, harmless without enforcement); emitted code never signs/authenticates x30 — it branches within the cache and `ret`s only through the exit thunk. |
| 11 | **The JIT becomes the project (again).** | The scope fence: no allocator, no IR, no speculation, fixed pinning, blocks only, C boundary untouched. M1 alone is shippable; every later milestone is a measured need, and §0's arithmetic defines "done" (~12–13 ms/f) — anything past M4 is refused by plan. |
| 12 | **Divergence found late in a long battery** (expensive to localize). | Deterministic compile ordinals + `TJ_ENG_JIT_MAXBLOCKS` bisect (log₂ of ~10⁴ blocks ≈ 14 runs); compile-time decode cross-check removes the decoder from suspicion; per-block dump + disasm; the frame number in the det diff bounds the trigger window. |

---

## 7. DECISIONS TAKEN (defaults chosen by this plan; overturn with evidence)

- **D1 — Threaded interpreter first (M1), not straight to emission.** It builds the
  block substrate the emitter needs anyway, is det-gated in days not weeks, speeds
  the qemu verification legs themselves, and is the permanent no-exec fallback.
- **D2 — Fixed pinning, no register allocator.** The 3–4x target is reachable
  without one; every avoided degree of freedom is verification budget saved.
- **D3 — The dispatch/GCALL boundary stays in C.** 1,675 crossings/frame ≈ 0.2 ms;
  burning it into emitted code buys nothing and costs every A/B switch.
- **D4 — x87 inlining generates FROM the FAST unit's committed paths and defers
  everything else to the unchanged unit.** One place owns x87 truth; the JIT owns
  only its speed.
- **D5 — JIT ships default-ON for the phone only after M2's device gates, per
  milestone thereafter** — the TJ_ENG_FAST precedent, including the headless det
  legs keeping their explicit-env discipline.
