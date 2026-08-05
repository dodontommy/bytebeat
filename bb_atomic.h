/* bb_atomic.h -- one atomic type that means the same thing in C and in C++.
 *
 * THE PROBLEM
 *
 * bytebeat.h describes memory that two threads share, and it is included by
 * both halves of this program: the engine (engine.c, expr.c, dsp.c, ...,
 * compiled as C11) and the GUI (app/*.cpp, compiled as C++17). Until now it
 * said `#include <stdatomic.h>` and `atomic_int` unconditionally, which is a C
 * spelling. `_Atomic` is a C keyword; it is not in C++ at all. Clang happens to
 * accept `_Atomic` in C++ as a language extension, which is the only reason
 * this ever built -- and it is why the project was Clang-only rather than
 * macOS-only. MSVC rejects it, and so does g++. MSVC does ship a
 * <stdatomic.h> for C++, but only under /std:c++23; we build the GUI as C++17,
 * and JUCE would not thank us for the jump.
 *
 * THE FIX
 *
 * Spell the shared fields BB_ATOMIC(T). In C that expands to _Atomic(T), which
 * is exactly what the engine had before, so the C side is bit-for-bit the code
 * it always was. In C++ it expands to std::atomic<T>, which is the portable
 * spelling every C++ compiler already implements.
 *
 * WHY THAT IS SAFE, AND HOW WE PROVE IT
 *
 * This only works because the two threads must agree on the LAYOUT of
 * `struct bb_state bb` -- the audio thread reaches it through C declarations
 * and the GUI reaches it through C++ declarations, and they had better be
 * describing the same bytes. For the scalar and pointer types this project
 * actually shares (int, unsigned, unsigned long long, and object pointers)
 * every implementation we target makes std::atomic<T> a naked T with the
 * hardware doing the work: same size, same alignment, always lock-free, value
 * at offset zero. That is not a promise the standard makes in so many words,
 * so we do not take it on faith. The _Static_assert / static_assert block
 * below checks size, alignment and lock-freedom for every type the shared
 * state uses, in BOTH languages, against the same expected values. If some
 * future toolchain decides std::atomic<long long> deserves a spinlock word or
 * a padded 16 bytes, this header stops the build with a message instead of
 * letting the GUI and the engine quietly read different fields of the same
 * struct. A wrong answer here is not a crash, it is a knob that silently moves
 * the wrong parameter at three in the morning, so it is worth the noise.
 *
 * WHY THE CALL SITES DID NOT HAVE TO CHANGE
 *
 * Roughly two hundred places in app/ already say atomic_load(&bb.rate) and
 * atomic_store(&bb.panic, 1) and atomic_load_explicit(&bb.bar,
 * memory_order_relaxed). Those are C spellings, but C++ has had exactly the
 * same free functions in namespace std since C++11 -- they exist so that C
 * headers ported to C++ keep working. So instead of writing our own shims
 * (which would collide with std's via argument-dependent lookup and make every
 * call ambiguous), we simply pull std's into the global namespace with
 * using-declarations. Unqualified lookup and ADL then both find the same
 * declarations, there is nothing to be ambiguous about, and not one line in
 * app/ needed editing.
 *
 * WHY THE extern "C++" WRAPPER
 *
 * Several headers in this project open `extern "C" {` and then #include other
 * headers from INSIDE it (rack.h, gen.h, expr.h, knob.h, dsp.h all do). If
 * bb_atomic.h is ever reached down one of those paths, <atomic> would be
 * dragged into a C-linkage region, and templates are not allowed to have C
 * language linkage -- the standard library would fail to compile in a way that
 * takes an afternoon to read. Linkage specifications nest, so wrapping our
 * includes in `extern "C++" { ... }` restores C++ linkage no matter what we
 * are nested inside. Combined with the include guard, which makes every
 * subsequent inclusion a no-op, this header is safe to reach from anywhere.
 * That is a seatbelt, not a fix: the headers listed above should still be
 * repaired to include their dependencies before opening extern "C".
 */
#ifndef BB_ATOMIC_H
#define BB_ATOMIC_H

#ifdef __cplusplus

/* ---- C++: std::atomic<T>, plus the C spellings the GUI already uses ------ */

extern "C++" {

#include <atomic>
#include <cstddef>

/* The memory orders. app/ writes these bare -- `memory_order_relaxed`, not
 * `std::memory_order_relaxed` -- because it was written against the C header.
 * Both spellings work after this; the qualified one is used in app/ too, on
 * the GUI's own std::atomic members. */
using std::memory_order;
using std::memory_order_relaxed;
using std::memory_order_consume;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_acq_rel;
using std::memory_order_seq_cst;

/* The generic free functions. Note these are using-declarations, not
 * definitions: `atomic_load(&bb.rate)` resolves to std::atomic_load, exactly
 * the overload ADL would have found anyway, so there is no second candidate
 * and no ambiguity. The list is deliberately wider than what app/ uses today
 * (which is load, store, load_explicit and exchange) so that adding a
 * fetch-add somewhere later does not send anyone back to this file. */
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_store;
using std::atomic_store_explicit;
using std::atomic_exchange;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub;
using std::atomic_fetch_sub_explicit;
using std::atomic_fetch_and;
using std::atomic_fetch_and_explicit;
using std::atomic_fetch_or;
using std::atomic_fetch_or_explicit;
using std::atomic_fetch_xor;
using std::atomic_fetch_xor_explicit;
using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_strong_explicit;
using std::atomic_compare_exchange_weak;
using std::atomic_compare_exchange_weak_explicit;
using std::atomic_thread_fence;
using std::atomic_signal_fence;

/* The convenience type names. Nothing in app/ spells these today -- the shared
 * struct is written in BB_ATOMIC(T) -- but engine.c, ui.c and audio.c use
 * atomic_int freely for their own file-static state, and if any of that ever
 * migrates into a header that C++ reads, it will find the name here. */
using std::atomic_bool;
using std::atomic_int;
using std::atomic_uint;
using std::atomic_long;
using std::atomic_ulong;
using std::atomic_llong;
using std::atomic_ullong;
using std::atomic_size_t;
using std::atomic_ptrdiff_t;
/* Deliberately NOT atomic_intptr_t / atomic_uintptr_t: the standard only
 * requires those when the implementation provides intptr_t, and nothing here
 * needs them. */

/* ---- the layout contract, checked ------------------------------------------
 * Every type below appears in `struct bb_state`, which the C engine and the
 * C++ GUI both map onto the same bytes. `is_always_lock_free` is the load
 * bearing one: a std::atomic<T> that is NOT lock-free carries a lock (or
 * indexes a global lock table) and can never be laid out like a C _Atomic(T)
 * that the engine is poking with hardware instructions. Size and alignment
 * then pin down the rest. */
static_assert (sizeof  (::std::atomic<int>) == sizeof  (int),
               "std::atomic<int> is not laid out like int; it cannot share "
               "struct bb_state with the C engine's _Atomic(int)");
static_assert (alignof (::std::atomic<int>) == alignof (int),
               "std::atomic<int> alignment differs from int");
static_assert (::std::atomic<int>::is_always_lock_free,
               "std::atomic<int> is not always lock-free on this target");

static_assert (sizeof  (::std::atomic<unsigned int>) == sizeof  (unsigned int),
               "std::atomic<unsigned> is not laid out like unsigned");
static_assert (alignof (::std::atomic<unsigned int>) == alignof (unsigned int),
               "std::atomic<unsigned> alignment differs from unsigned");
static_assert (::std::atomic<unsigned int>::is_always_lock_free,
               "std::atomic<unsigned> is not always lock-free on this target");

static_assert (sizeof  (::std::atomic<unsigned long long>)
                   == sizeof  (unsigned long long),
               "std::atomic<unsigned long long> is not laid out like the "
               "underlying type (bb.epoch is read across the language border)");
static_assert (alignof (::std::atomic<unsigned long long>)
                   == alignof (unsigned long long),
               "std::atomic<unsigned long long> alignment differs");
static_assert (::std::atomic<unsigned long long>::is_always_lock_free,
               "std::atomic<unsigned long long> is not always lock-free; on a "
               "32-bit target without a 64-bit CAS this would take a lock in "
               "the audio thread, which is forbidden");

/* Stands in for every _Atomic(X *) in the shared state: Layer.prog is
 * _Atomic(Program *) and the engine's file statics hold _Atomic(SmpBuf *),
 * _Atomic(ArrSong *) and _Atomic(int16_t *). All object pointers have one
 * representation on every target we build for, so one check covers them. */
static_assert (sizeof  (::std::atomic<void *>) == sizeof  (void *),
               "std::atomic<T*> is not laid out like a pointer; the Program "
               "hand-off between the UI and audio threads would not survive");
static_assert (alignof (::std::atomic<void *>) == alignof (void *),
               "std::atomic<T*> alignment differs from a plain pointer");
static_assert (::std::atomic<void *>::is_always_lock_free,
               "std::atomic<T*> is not always lock-free on this target");

}   /* extern "C++" */

/* The parentheses around T are deliberate: BB_ATOMIC(Program *) has to work,
 * and the leading :: keeps us honest if this is ever expanded inside a
 * namespace that has its own `std`. */
#define BB_ATOMIC(T) ::std::atomic< T >

#else /* !__cplusplus */

/* ---- C: exactly what the engine always had ------------------------------ */

#include <stdatomic.h>

#define BB_ATOMIC(T) _Atomic(T)

/* The mirror image of the C++ checks above, against the same expected values.
 * The engine is the side that runs in the audio thread, so if either language
 * is going to be told off for a surprising layout it had better be told at
 * compile time and not by an xrun.
 *
 * These go through typedefs rather than being written as
 * _Alignof(_Atomic(int)) inline, because that spelling puts a type-specifier
 * inside a _Alignof and not every C front end is happy about parsing it. */
typedef _Atomic(int)                bb_atomic_probe_int;
typedef _Atomic(unsigned int)       bb_atomic_probe_uint;
typedef _Atomic(unsigned long long) bb_atomic_probe_ullong;
typedef _Atomic(void *)             bb_atomic_probe_ptr;

_Static_assert (sizeof  (bb_atomic_probe_int) == sizeof  (int),
                "_Atomic(int) is not laid out like int; struct bb_state would "
                "not match the C++ GUI's view of it");
_Static_assert (_Alignof (bb_atomic_probe_int) == _Alignof (int),
                "_Atomic(int) alignment differs from int");

_Static_assert (sizeof  (bb_atomic_probe_uint) == sizeof  (unsigned int),
                "_Atomic(unsigned) is not laid out like unsigned");
_Static_assert (_Alignof (bb_atomic_probe_uint) == _Alignof (unsigned int),
                "_Atomic(unsigned) alignment differs from unsigned");

_Static_assert (sizeof  (bb_atomic_probe_ullong) == sizeof (unsigned long long),
                "_Atomic(unsigned long long) is not laid out like the "
                "underlying type; bb.epoch crosses the language border");
_Static_assert (_Alignof (bb_atomic_probe_ullong)
                    == _Alignof (unsigned long long),
                "_Atomic(unsigned long long) alignment differs");

_Static_assert (sizeof  (bb_atomic_probe_ptr) == sizeof  (void *),
                "_Atomic(T*) is not laid out like a pointer; the Program "
                "hand-off between the UI and audio threads would not survive");
_Static_assert (_Alignof (bb_atomic_probe_ptr) == _Alignof (void *),
                "_Atomic(T*) alignment differs from a plain pointer");

/* Lock-freedom, where the implementation bothers to tell us. The macro is 0
 * for "never lock-free", 1 for "sometimes", 2 for "always". Zero is the case
 * that matters: it means the audio thread would take a lock on every knob
 * read, which engine.h:32-35 forbids outright.
 *
 * We check for 0 rather than insisting on 2, because MSVC's <stdatomic.h>
 * reports 1 for every one of these on x64 even though the types genuinely are
 * lock-free -- atomic_is_lock_free() returns true for all of them at runtime,
 * and the C++ branch above passes std::atomic<T>::is_always_lock_free, which
 * is the same compiler making the strict statement about the same ABI. So the
 * strong claim IS being asserted, over in C++; asking for 2 here as well would
 * only be asserting how chatty this particular header chose to be.
 *
 * Guarded on #ifdef rather than assumed present, because an implementation
 * that omitted the macro would otherwise fail here for the wrong reason. */
#if defined(ATOMIC_INT_LOCK_FREE)
_Static_assert (ATOMIC_INT_LOCK_FREE != 0,
                "_Atomic int is never lock-free on this target; the audio "
                "thread would take a lock, which it is never allowed to do");
#endif
#if defined(ATOMIC_LLONG_LOCK_FREE)
_Static_assert (ATOMIC_LLONG_LOCK_FREE != 0,
                "_Atomic long long is never lock-free on this target; "
                "bb.epoch is incremented by the audio thread every period");
#endif
#if defined(ATOMIC_POINTER_LOCK_FREE)
_Static_assert (ATOMIC_POINTER_LOCK_FREE != 0,
                "_Atomic pointers are never lock-free on this target; "
                "publishing a Program to the audio thread would take a lock");
#endif

#endif /* __cplusplus */

#endif /* BB_ATOMIC_H */
