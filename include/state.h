/* state.h — single source of truth for all kernel runtime state.
 *
 * PURPOSE
 *   Centralise every piece of mutable kernel state that was previously
 *   scattered as file-scope statics across net/chat.c, net/faultchat.c,
 *   tools/patch_tools.c, tools/telegram_tools.c and kernel/main.c into one
 *   zero-initialised global, g_state.  This gives the operator and tests a
 *   single, coherent view of the machine's runtime condition and makes every
 *   state transition explicit rather than an implicit side-effect of a
 *   function call.
 *
 * ARCHITECTURE — Redux-style in C
 *   - ONE global:   kernel_state_t g_state  (in .bss, zero-initialised)
 *   - ACTIONS:      action_t — a tagged union that names and carries the
 *                   payload of every legal state transition.
 *   - REDUCERS:     four reduce_X() functions in core/state.c — the ONLY
 *                   functions that write to g_state, reached through
 *                   state_dispatch().
 *   - SELECTORS:    unrestricted direct reads: g_state.domain.field.
 *                   No lock needed; the cooperative scheduler is single-
 *                   threaded and never pre-empts.
 *
 *   Fine-grained internal bookkeeping that has no externally observable
 *   meaning is exempt: the history arena helpers in net/chat.c and the RIP
 *   table helpers in net/faultchat.c mutate their sub-structs directly
 *   rather than dispatching one action per byte.  Actions cover transitions
 *   at a granularity the operator would recognise ("a turn began", "a patch
 *   was applied"), not bookkeeping that only the module understands.
 *
 * RESPONSIBILITIES
 *   - Define chat_hist_msg_t and the four domain state structs.
 *   - Define kernel_state_t, the single aggregated state.
 *   - Define action_type_t, per-action payload structs, and action_t.
 *   - Export g_state and state_dispatch().
 *   - Export patch_slot_t and the STATE_PATCH_* constants that tools/
 *     patch_tools.c previously owned privately.
 *
 * PUBLIC API
 *   g_state              Read any field from anywhere; no accessor needed.
 *   state_dispatch(a)    Route an action to all four domain reducers.
 *
 * DEPENDENCIES
 *   chat.h, faultchat.h, model.h (one-way; those headers do not include
 *   state.h).  Freestanding-safe; compiles on the host test binary without
 *   any guards.
 *
 * FUTURE EXTENSION POINTS
 *   - Snapshot: memcpy g_state to persistent storage and restore on reboot.
 *   - Phase 2: add fiber_state_t, capability_state_t, agenda_state_t.
 *   - Structured log: every action_t is already a serialisable record;
 *     appending them to a VFS file would give a machine-readable audit trail
 *     without touching any of the existing interfaces.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* chat.h brings in model.h, tool.h (TOOL_NAME_MAX, CHAT_ACTION_DETAIL_MAX)
 * and all CHAT_* constants.  faultchat.h brings in fault.h and all
 * FAULTCHAT_* constants.  model.h defines model_transport_t. */
#include "chat.h"
#include "faultchat.h"
#include "model.h"

/* ====================================================================== */
/* Patch domain — constants and struct (moved from tools/patch_tools.c)   */
/* ====================================================================== */

#define STATE_PATCH_MAX_BYTES  64   /* maximum bytes per live patch        */
#define STATE_PATCH_MAX_SLOTS   8   /* maximum concurrently active patches */
#define STATE_PATCH_NAME_MAX   64   /* max length of an exported symbol    */

/* One live code patch and its rollback data. */
typedef struct {
    char      name[STATE_PATCH_NAME_MAX]; /* exported symbol name          */
    uintptr_t addr;                        /* address that was patched      */
    uint8_t   orig[STATE_PATCH_MAX_BYTES]; /* original bytes before patch  */
    uint8_t   patch_bytes[STATE_PATCH_MAX_BYTES]; /* bytes written         */
    uint8_t   len;                         /* number of bytes patched       */
    uint8_t   active;                      /* 1 if this slot is in use      */
} patch_slot_t;

/* ====================================================================== */
/* chat_hist_msg_t — moved here from net/chat.c                           */
/* ====================================================================== */

/* One remembered history message.  off/len locate the raw JSON content
 * in chat_state_t.arena; epoch is the operator turn (the unit of eviction,
 * see chat.h).  role points to a .rodata literal in net/chat.c and is
 * always valid for the lifetime of the kernel. */
typedef struct {
    uint32_t    off;
    uint32_t    len;
    uint16_t    epoch;
    const char *role;
} chat_hist_msg_t;

/* ====================================================================== */
/* Domain state structs                                                    */
/* ====================================================================== */

typedef struct {
    /* History storage — mutated directly by hist_* helpers in net/chat.c */
    char            arena[CHAT_HISTORY_BYTES];
    chat_hist_msg_t msgs[CHAT_HISTORY_MSGS];
    size_t          msgs_n;
    size_t          arena_used;
    uint16_t        epoch;        /* operator turn currently being run     */

    /* Re-entrancy guard */
    int             in_turn;

    /* Per-turn statistics (reset at the start of each chat_ask call) */
    unsigned        stat_rounds;
    unsigned        stat_tool_calls;
    unsigned        stat_evictions;  /* total since init; not per-turn     */
    unsigned        stat_retries;

    /* Action journal — what this machine actually dispatched */
    chat_action_t   journal[CHAT_JOURNAL_ENTRIES];
    unsigned        journal_total;   /* tool calls dispatched since boot   */

    /* Tool schema cache */
    char            tools_buf[CHAT_TOOLS_BYTES];
    size_t          tools_len;
    int             tools_ready;

    /* Configurable round cap; default CHAT_MAX_ROUNDS, 0 means default */
    unsigned        max_rounds;

    /* Transport binding — set once at boot, not reducer-driven */
    model_transport_t *transport;
} chat_state_t;

typedef struct {
    int64_t  active_chat_id;  /* chat_id of the current Telegram session  */
    uint64_t next_poll_ms;    /* monotonic ms: time of next poll           */
    int      sent;            /* 1 if telegram_send was called this turn   */
} telegram_state_t;

typedef struct {
    patch_slot_t slots[STATE_PATCH_MAX_SLOTS];
} patch_state_t;

typedef struct {
    /* Guard flags */
    int             enabled;    /* policy switch; 1 on reset/bind         */
    int             busy;       /* re-entrancy latch during a pump        */
    int             off;        /* permanently latched off this boot      */
    const char     *off_why;   /* .rodata string; "" when not latched     */

    /* Counters */
    uint32_t        seen;        /* fault_count() as of last pump          */
    uint32_t        diagnoses;   /* attempts sent (including failures)     */
    uint32_t        fixes;       /* proposed fixes that were armed         */
    uint32_t        patches;     /* proposed code patches applied          */

    /* Per-address attempt table — mutated directly by rip_* helpers in
     * net/faultchat.c, for the same reason the history arena is exempt */
    struct { uint64_t rip; uint32_t tries; } rip_table[4];
    uint32_t        rip_next;    /* round-robin insertion index            */

    /* Last outcome */
    int             last_result; /* FAULTCHAT_* code                       */
    uint32_t        last_seq;    /* which fault record was last diagnosed  */
    char            last_why[FAULTCHAT_WHY_MAX];

    /* Last parsed reply */
    faultchat_reply_t last_reply;

    /* Transport binding — set once at boot, not reducer-driven */
    model_transport_t *transport;
} faultchat_state_t;

/* The single aggregated kernel state. */
typedef struct {
    chat_state_t      chat;
    telegram_state_t  telegram;
    patch_state_t     patch;
    faultchat_state_t faultchat;
} kernel_state_t;

/* ====================================================================== */
/* Actions                                                                  */
/* ====================================================================== */

typedef enum {
    /* ---- chat domain ---- */
    ACT_CHAT_INIT,             /* bind transport; clear everything         */
    ACT_CHAT_RESET,            /* forget conversation; keep config         */
    ACT_CHAT_TURN_BEGIN,       /* set in_turn, epoch, reset per-turn stats */
    ACT_CHAT_TURN_END,         /* clear in_turn                            */
    ACT_CHAT_ROUND_COMPLETE,   /* stat_rounds++                            */
    ACT_CHAT_TOOL_DISPATCHED,  /* stat_tool_calls++                        */
    ACT_CHAT_EVICTION,         /* stat_evictions++                         */
    ACT_CHAT_RETRY,            /* stat_retries++                           */
    ACT_CHAT_JOURNAL_RECORD,   /* append one entry to the action ring      */
    ACT_CHAT_TOOLS_LOADED,     /* record assembled schema size             */
    ACT_CHAT_SET_MAX_ROUNDS,   /* override the round cap                   */

    /* ---- telegram domain ---- */
    ACT_TG_MESSAGE_RECEIVED,   /* set active_chat_id                       */
    ACT_TG_TURN_COMPLETE,      /* clear active_chat_id; reset sent flag    */
    ACT_TG_SENT,               /* mark sent=1                              */
    ACT_TG_POLL_SCHEDULED,     /* update next_poll_ms                      */

    /* ---- patch domain ---- */
    ACT_PATCH_APPLY,           /* fill a free rollback slot                */
    ACT_PATCH_ROLLBACK,        /* clear a rollback slot by name            */

    /* ---- faultchat domain ---- */
    ACT_FAULTCHAT_BIND,             /* bind transport then reset            */
    ACT_FAULTCHAT_RESET,            /* clear all state; set enabled=1       */
    ACT_FAULTCHAT_ENABLE,           /* policy on/off switch                 */
    ACT_FAULTCHAT_PUMP_BEGIN,       /* set busy=1                           */
    ACT_FAULTCHAT_PUMP_END,         /* set busy=0; record result+seq+why    */
    ACT_FAULTCHAT_LATCH_OFF,        /* permanently disable for this boot    */
    ACT_FAULTCHAT_DIAGNOSIS_SENT,   /* diagnoses++; update seen             */
    ACT_FAULTCHAT_FIX_ARMED,        /* fixes++                              */
    ACT_FAULTCHAT_PATCH_APPLIED,    /* patches++                            */
    ACT_FAULTCHAT_REPLY_STORED,     /* copy reply into last_reply           */
} action_type_t;

/* ---- payload structs ---- */

/* chat */
typedef struct { model_transport_t *transport; } act_chat_init_t;
typedef struct { uint16_t epoch; }               act_chat_turn_begin_t;
typedef struct {
    char name[TOOL_NAME_MAX];
    char detail[CHAT_ACTION_DETAIL_MAX];
    int  failed;
} act_chat_journal_record_t;
typedef struct { size_t   tools_len; } act_chat_tools_loaded_t;
typedef struct { unsigned n; }         act_chat_set_max_rounds_t;

/* telegram */
typedef struct { int64_t  chat_id; }  act_tg_message_received_t;
typedef struct { uint64_t next_ms; }  act_tg_poll_scheduled_t;

/* patch */
typedef struct {
    char      name[STATE_PATCH_NAME_MAX];
    uintptr_t addr;
    uint8_t   orig[STATE_PATCH_MAX_BYTES];
    uint8_t   patch_bytes[STATE_PATCH_MAX_BYTES];
    uint8_t   len;
} act_patch_apply_t;
typedef struct {
    char name[STATE_PATCH_NAME_MAX];
} act_patch_rollback_t;

/* faultchat */
typedef struct {
    model_transport_t *transport;
    uint32_t           seen;   /* fault_count() at bind time — caller supplies */
} act_faultchat_bind_t;
typedef struct {
    uint32_t seen;             /* fault_count() at reset time                   */
} act_faultchat_reset_t;
typedef struct { int on; }              act_faultchat_enable_t;
typedef struct {
    int      result;
    uint32_t seq;
    char     why[FAULTCHAT_WHY_MAX];
} act_faultchat_pump_end_t;
typedef struct { const char *why; }     act_faultchat_latch_off_t;
typedef struct { uint32_t new_seen; }   act_faultchat_diagnosis_sent_t;
typedef struct { faultchat_reply_t reply; } act_faultchat_reply_stored_t;

/* Tagged union.  Zero-payload actions use {.type = ACT_X} with no .u
 * member accessed. */
typedef struct {
    action_type_t type;
    union {
        act_chat_init_t           chat_init;
        act_chat_turn_begin_t     chat_turn_begin;
        act_chat_journal_record_t chat_journal_record;
        act_chat_tools_loaded_t   chat_tools_loaded;
        act_chat_set_max_rounds_t chat_set_max_rounds;

        act_tg_message_received_t tg_message_received;
        act_tg_poll_scheduled_t   tg_poll_scheduled;

        act_patch_apply_t    patch_apply;
        act_patch_rollback_t patch_rollback;

        act_faultchat_bind_t            faultchat_bind;
        act_faultchat_reset_t           faultchat_reset;
        act_faultchat_enable_t          faultchat_enable;
        act_faultchat_pump_end_t        faultchat_pump_end;
        act_faultchat_latch_off_t       faultchat_latch_off;
        act_faultchat_diagnosis_sent_t  faultchat_diagnosis_sent;
        act_faultchat_reply_stored_t    faultchat_reply_stored;
    } u;
} action_t;

/* ====================================================================== */
/* Global state and dispatch                                               */
/* ====================================================================== */

/* The single source of truth.  Lives in .bss; zero-initialised on boot.
 *
 * Non-zero defaults that zero-init cannot express:
 *   faultchat.enabled  = 1           (set by ACT_FAULTCHAT_RESET reducer)
 *   faultchat.last_result = FAULTCHAT_ENONE  (same)
 *   faultchat.off_why  = ""          (same — NULL would crash on print)
 *   chat.max_rounds    = CHAT_MAX_ROUNDS (set by ACT_CHAT_INIT reducer)
 *
 * Read directly anywhere: g_state.domain.field.
 * Write ONLY through state_dispatch() — never by direct assignment. */
extern kernel_state_t g_state;

/* Route an action to all four domain reducers.  NULL is silently ignored. */
void state_dispatch(const action_t *a);
