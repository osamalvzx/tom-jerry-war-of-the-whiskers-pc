// The host->guest call seam (Stage 3, port/ANDROID_PLAN.md). Every place a hybrid hook
// calls INTO guest code by address must go through GuestFnPtr so the target stays
// interpreted when TJ_ENGINE=1 — a direct literal call would execute the guest natively
// inside the escape, silently un-interpreting the game.
//
//   before:  ((FnGetText)0x19910)(id)
//   after:   ((FnGetText)GuestFnPtr(0x19910))(id)
//
// Native mode (TJ_ENGINE unset): GuestFnPtr returns the raw address — the shipped
// v1.1.0 behavior, bit for bit. The call-site CAST carries the calling convention in
// both modes; keep it the ORIGINAL function's convention (the S1b lesson).
//
// On ARM (Stage 5) this same seam becomes Engine::Call — the sites will not change again.
#pragma once
#include <cstdint>
#include "hybrid/dispatch.h"

namespace tj::hybrid {
void* GuestFnPtr(uint32_t va);   // engine_mode.cpp
bool  EngineModeArmed();
// The guest return address the CURRENT (innermost) host escape displaced with its
// capture thunk, 0 if no escape is active. Diagnostics only: a hook that wants the true
// guest caller must use this when [esp] holds the thunk instead (engine mode).
uint32_t EngineModeDisplacedRet();

// CONTINUATION HOOKS — portable C semantics for the naked wrappers. Two shapes:
//  * jmp-in/jmp-out (lan_match's Hk_RoundEnd over the 7-byte instruction at 0x172EC):
//    [esp] is live guest DATA — the escape's displacement handed the guest a thunk
//    address as a heap pointer (the frame-10,645 crash). escapeSafe = false: the
//    semantic is the ONLY correct engine-mode path.
//  * call-shaped naked hooks (net_sync's Hk_MtNext / Hk_Mode6Begin): the escape+naked
//    path works on the x86 host but cannot exist on ARM. escapeSafe = true: the
//    semantic is the portable path, and TJ_ENG_RAWHOOKS=1 reverts to escape+naked for
//    same-binary A/B.
// The engine invokes `fn(s)` bracketed like a dispatch hook (frame push + x87
// materialization), so the semantic may read guest state through s and MAY call guest
// code via GuestCall; it then resumes interpreting at resumeVa. Native mode never
// reads this table — the naked wrappers remain the native path.
void EngineModeRegisterJmpHook(const void* hookEntry, uint32_t resumeVa,
                               void(__cdecl* fn)(tj::engine::CpuState&),
                               bool escapeSafe = false);

// Any hook that REWRITES GUEST CODE BYTES at runtime (patch toggles) must report the
// range here after writing: the engine's decode cache invalidates on interpreted
// stores automatically, but native writes are invisible to it. No-op in native mode.
void EngineModeInvalidateCode(uint32_t va, uint32_t len);

// ApplyFsFixups allocates the fake KPCR after the engine may already be armed (ARM
// boots armed); this points the interpreter's fs base at it. Idempotent, both hosts.
void EngineModeRefreshFsBase();

#if !defined(_M_IX86)
// ARM (headless_main): arm the engine BEFORE startup init — guest stack + exec range +
// escape handler + virtual FPU + a live root dispatch frame, so every GCALL from host
// boot code marshals from process start. There is no native mode to fall back to.
bool EngineModeArmBoot();
#endif

// ================= Engine::Call — the marshaled host->guest call =================
//
// GCALL(Conv, FnTypedef, va, args...) replaces ((FnTypedef)GuestFnPtr(va))(args...).
// The typedef keeps carrying the ORIGINAL function's signature; the Conv tag records
// its convention as data (static_asserted against the typedef on x86 — the same
// machine-check the HOOK_* markers give the other direction; on ARM, where the
// typedef loses its convention, the tag is authoritative).
//
// Behavior by context:
//  * native mode (TJ_ENGINE unset): exactly the old expression — the typed cast, the
//    compiler-emitted call. Bit-identical v1.1.0.
//  * engine mode, hook invoked through the DISPATCH table: marshal — build a guest
//    frame below the live interpreted ESP, nested Run to a sentinel, assert the
//    callee's stack delta against the tag (GuestMarshalCall). The ARM shape.
//  * engine mode, escape context (TJ_ENG_NODISPATCH, jmp-hooks) or TJ_ENG_GATECALL=1:
//    the proven x86 gate via GuestFnPtr — the legacy path, kept for same-binary A/B.
//
// Float args ride the stack as bit patterns (never ecx/edx — MSVC fastcall register
// eligibility is integer-class only, asserted below); a float RETURN materializes on
// the host FPU (the gate's own behavior) and is popped here, mirroring the native
// caller's fstp.
namespace gcall_detail {

template <class T> inline uint32_t ToSlot(T v) {
    if constexpr (std::is_pointer_v<T>) {
        // A pointer argument handed to guest code must be a guest-visible address.
        uintptr_t u = (uintptr_t)v;
#if !defined(_M_IX86)
        if (u >> 32) tj::hybrid::dispatch_detail::DispatchFatal(
            "guest-call pointer argument above 4 GB (relocate the data)");
#endif
        return (uint32_t)u;
    } else {
        static_assert(sizeof(T) <= 4, "guest-call args must fit one 4-byte slot");
        uint32_t u = 0; std::memcpy(&u, &v, sizeof(T)); return u;
    }
}

} // namespace gcall_detail

template <CallConv CC, class Fn, size_t... I, class... A>
typename dispatch_detail::SigFor<CC, Fn>::Ret
GuestCallMarshal(uint32_t va, std::index_sequence<I...>, A... a) {
    using Sig = dispatch_detail::SigFor<CC, Fn>;
    using R = typename Sig::Ret;
    constexpr int n = Sig::nargs;

    uint32_t slot[n > 0 ? n : 1];
    ((slot[I] = gcall_detail::ToSlot(
          static_cast<std::tuple_element_t<I, typename Sig::ArgTuple>>(a))), ...);

    uint32_t ecx = 0, edx = 0;
    uint32_t stackW[n > 0 ? n : 1];
    int nStack = 0;
    if constexpr (CC == CallConv::Fastcall) {
        // MSVC fastcall: first two DWORD-or-smaller INTEGER-class args go in ecx/edx.
        // Every fastcall typedef in this codebase leads with two integer args — assert
        // it so the simple assignment below stays honest.
        static_assert(n >= 2 &&
            !std::is_floating_point_v<std::tuple_element_t<0, typename Sig::ArgTuple>> &&
            !std::is_floating_point_v<std::tuple_element_t<1, typename Sig::ArgTuple>>,
            "fastcall guest call: first two args must be integer-class");
        ecx = slot[0]; edx = slot[1];
        for (int i = 2; i < n; ++i) stackW[nStack++] = slot[i];
    } else {
        for (int i = 0; i < n; ++i) stackW[nStack++] = slot[i];
    }

    const uint32_t eax = GuestMarshalCall(va, stackW, nStack, ecx, edx,
                                          Sig::cleanBytes, "gcall");
    if constexpr (std::is_same_v<R, float>) { (void)eax; return GuestMarshalPopF32(); }
    else if constexpr (std::is_void_v<R>)   { (void)eax; return; }
    else if constexpr (std::is_pointer_v<R>) return (R)(uintptr_t)eax;
    else                                     return (R)eax;
}

template <CallConv CC, class Fn, class... A>
typename dispatch_detail::SigFor<CC, Fn>::Ret GuestCall(uint32_t va, A... a) {
    using Sig = dispatch_detail::SigFor<CC, Fn>;
    using R = typename Sig::Ret;
#if defined(_M_IX86)
    static_assert(Sig::conv == CC, "GCALL tag mismatches the typedef's calling convention");
#endif
    static_assert((int)sizeof...(A) == Sig::nargs, "GCALL arg count mismatches the typedef");
    static_assert(dispatch_detail::RetOk<R>::value || std::is_same_v<R, float>,
        "guest-call return type not marshalable (double/64-bit) — extend deliberately");

    if (!GuestMarshalReady()) {
#if defined(_M_IX86)
        return ((Fn)(uintptr_t)GuestFnPtr(va))(a...);   // native mode, or gate contexts
#else
        // ARM has no native mode and no gates: a guest call without a live marshal
        // context is a design violation, not a fallback situation.
        dispatch_detail::DispatchFatal("GuestCall with no marshal context");
#endif
    }

    // Convert each argument to its DECLARED type first (what the native call's caller-
    // side conversions did), then classify: fastcall puts the first two INTEGER-class
    // declared args in ecx/edx; everything else is a stack slot, left-to-right.
    return GuestCallMarshal<CC, Fn>(va, std::make_index_sequence<sizeof...(A)>{}, a...);
}

// The registration-shaped marker, symmetric to HOOK_*:
//   GCALL(Fastcall, FnThisU32, 0x19E70, item, 0, idx)
#define GCALL(conv, Fn, va, ...) \
    (tj::hybrid::GuestCall<tj::hybrid::CallConv::conv, Fn>((va), __VA_ARGS__))
#define GCALL0(conv, Fn, va) \
    (tj::hybrid::GuestCall<tj::hybrid::CallConv::conv, Fn>((va)))

// Call a function-pointer VALUE read from a guest vtable: guest .text addresses go
// through the seam; anything else (a host address an install wrote there) is called
// raw, exactly as before.
template <CallConv CC, class Fn, class... A>
typename dispatch_detail::SigFor<CC, Fn>::Ret GuestVtCall(Fn f, A... a) {
    uint32_t va = (uint32_t)(uintptr_t)f;
    if (va < 0x000EE000u) return GuestCall<CC, Fn>(va, a...);
    return f(a...);
}
#define GVCALL(conv, Fn, fnval, ...) \
    (tj::hybrid::GuestVtCall<tj::hybrid::CallConv::conv, Fn>((fnval), __VA_ARGS__))
}
