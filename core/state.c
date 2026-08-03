/* core/state.c — global kernel state definition and reducer dispatch.
 *
 * PURPOSE
 *   Define g_state (the single source of truth for all mutable kernel
 *   runtime state) and route every action_t to the appropriate domain
 *   reducer.  See include/state.h for the architecture description.
 *
 * RESPONSIBILITIES
 *   - Define kernel_state_t g_state in .bss (zero-initialised at boot).
 *   - Implement state_dispatch() which calls the four reduce_X() statics.
 *   - Implement the four reduce_X() functions — the ONLY writers to g_state.
 *   - Assert that STATE_PATCH_* constants match what patch_tools.c expected,
 *     so the two cannot drift silently.
 *
 * DEPENDENCIES
 *   include/state.h (which pulls in chat.h, faultchat.h, model.h).
 *   No hardware, no lwIP, no allocation — compiles on the host test binary
 *   without any FABLEOS_HOSTTEST guards.
 */

#include "state.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The one global.  Zero-initialised in .bss.  Non-zero defaults are set by
 * the ACT_CHAT_INIT and ACT_FAULTCHAT_RESET/BIND reducers. */
kernel_state_t g_state;

/* ====================================================================== */
/* Sanity checks — keep constants in sync with patch_tools.c              */
/* ====================================================================== */

_Static_assert(STATE_PATCH_MAX_SLOTS == 8,
    "patch slot count changed; patch_tools.c expected 8");
_Static_assert(STATE_PATCH_MAX_BYTES == 64,
    "patch byte limit changed; patch_tools.c expected 64");
_Static_assert(STATE_PATCH_NAME_MAX == 64,
    "patch name limit changed; patch_tools.c expected 64");

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

/* Bounded copy that always NUL-terminates dst.  No strncpy padding. */
static void bstr_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* String equality without libc (safe on bare kernel or host). */
static int bstr_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Empty string in .rodata so faultchat.off_why is never NULL. */
static const char fc_empty[] = "";

/* ====================================================================== */
/* reduce_chat                                                             */
/* ====================================================================== */

static void reduce_chat(kernel_state_t *s, const action_t *a)
{
    chat_state_t *c = &s->chat;
    switch (a->type) {

    case ACT_CHAT_INIT:
        c->transport      = a->u.chat_init.transport;
        c->msgs_n         = 0;
        c->arena_used     = 0;
        c->epoch          = 0;
        c->in_turn        = 0;
        c->stat_rounds    = 0;
        c->stat_tool_calls = 0;
        c->stat_evictions = 0;
        c->stat_retries   = 0;
        c->journal_total  = 0;
        c->tools_ready    = 0;
        c->tools_len      = 0;
        c->max_rounds     = CHAT_MAX_ROUNDS;
        break;

    case ACT_CHAT_RESET:
        c->msgs_n     = 0;
        c->arena_used = 0;
        c->epoch      = 0;
        break;

    case ACT_CHAT_TURN_BEGIN:
        c->in_turn         = 1;
        c->epoch           = a->u.chat_turn_begin.epoch;
        c->stat_rounds     = 0;
        c->stat_tool_calls = 0;
        c->stat_retries    = 0;
        break;

    case ACT_CHAT_TURN_END:
        c->in_turn = 0;
        break;

    case ACT_CHAT_ROUND_COMPLETE:
        c->stat_rounds++;
        break;

    case ACT_CHAT_TOOL_DISPATCHED:
        c->stat_tool_calls++;
        break;

    case ACT_CHAT_EVICTION:
        c->stat_evictions++;
        break;

    case ACT_CHAT_RETRY:
        c->stat_retries++;
        break;

    case ACT_CHAT_JOURNAL_RECORD: {
        const act_chat_journal_record_t *p = &a->u.chat_journal_record;
        c->journal_total++;
        unsigned idx = (c->journal_total - 1) % CHAT_JOURNAL_ENTRIES;
        chat_action_t *e = &c->journal[idx];
        e->seq    = c->journal_total;
        e->turn   = (uint16_t)c->epoch;
        e->failed = (uint8_t)p->failed;
        bstr_copy(e->name,   p->name,   sizeof e->name);
        bstr_copy(e->detail, p->detail, sizeof e->detail);
        break;
    }

    case ACT_CHAT_TOOLS_LOADED:
        c->tools_len   = a->u.chat_tools_loaded.tools_len;
        c->tools_ready = 1;
        break;

    case ACT_CHAT_SET_MAX_ROUNDS: {
        unsigned n = a->u.chat_set_max_rounds.n;
        c->max_rounds = (n == 0 || n > CHAT_MAX_ROUNDS) ? CHAT_MAX_ROUNDS : n;
        break;
    }

    default:
        break;
    }
}

/* ====================================================================== */
/* reduce_telegram                                                         */
/* ====================================================================== */

static void reduce_telegram(kernel_state_t *s, const action_t *a)
{
    telegram_state_t *t = &s->telegram;
    switch (a->type) {

    case ACT_TG_MESSAGE_RECEIVED:
        t->active_chat_id = a->u.tg_message_received.chat_id;
        break;

    case ACT_TG_TURN_COMPLETE:
        t->active_chat_id = 0;
        t->sent           = 0;
        break;

    case ACT_TG_SENT:
        t->sent = 1;
        break;

    case ACT_TG_POLL_SCHEDULED:
        t->next_poll_ms = a->u.tg_poll_scheduled.next_ms;
        break;

    default:
        break;
    }
}

/* ====================================================================== */
/* reduce_patch                                                            */
/* ====================================================================== */

static void reduce_patch(kernel_state_t *s, const action_t *a)
{
    patch_state_t *p = &s->patch;
    switch (a->type) {

    case ACT_PATCH_APPLY: {
        const act_patch_apply_t *ap = &a->u.patch_apply;
        /* Find a free slot.  Caller must have verified space exists. */
        patch_slot_t *slot = (patch_slot_t *)0;
        for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
            if (!p->slots[i].active) { slot = &p->slots[i]; break; }
        }
        if (!slot) break;
        bstr_copy(slot->name, ap->name, sizeof slot->name);
        slot->addr   = ap->addr;
        slot->len    = ap->len;
        slot->active = 1;
        unsigned n = ap->len < STATE_PATCH_MAX_BYTES
                     ? ap->len : STATE_PATCH_MAX_BYTES;
        memcpy(slot->orig,        ap->orig,        n);
        memcpy(slot->patch_bytes, ap->patch_bytes, n);
        break;
    }

    case ACT_PATCH_ROLLBACK: {
        const char *name = a->u.patch_rollback.name;
        for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
            if (p->slots[i].active && bstr_eq(p->slots[i].name, name)) {
                memset(&p->slots[i], 0, sizeof p->slots[i]);
                break;
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ====================================================================== */
/* reduce_faultchat                                                        */
/* ====================================================================== */

/* Clear all faultchat state except transport and write the non-zero
 * defaults.  Used by both ACT_FAULTCHAT_BIND and ACT_FAULTCHAT_RESET. */
static void fc_do_reset(faultchat_state_t *f, uint32_t seen)
{
    f->busy        = 0;
    f->off         = 0;
    f->off_why     = fc_empty;
    f->seen        = seen;
    f->diagnoses   = 0;
    f->fixes       = 0;
    f->patches     = 0;
    f->rip_next    = 0;
    f->last_result = FAULTCHAT_ENONE;
    f->last_seq    = 0;
    f->last_why[0] = '\0';
    for (int i = 0; i < 4; i++) {
        f->rip_table[i].rip   = 0;
        f->rip_table[i].tries = 0;
    }
    memset(&f->last_reply, 0, sizeof f->last_reply);
    f->enabled = 1;   /* on by default; set last so it cannot be shadowed */
}

static void reduce_faultchat(kernel_state_t *s, const action_t *a)
{
    faultchat_state_t *f = &s->faultchat;
    switch (a->type) {

    case ACT_FAULTCHAT_BIND:
        f->transport = a->u.faultchat_bind.transport;
        fc_do_reset(f, a->u.faultchat_bind.seen);
        break;

    case ACT_FAULTCHAT_RESET:
        fc_do_reset(f, a->u.faultchat_reset.seen);
        break;

    case ACT_FAULTCHAT_ENABLE:
        f->enabled = a->u.faultchat_enable.on ? 1 : 0;
        break;

    case ACT_FAULTCHAT_PUMP_BEGIN:
        f->busy = 1;
        break;

    case ACT_FAULTCHAT_PUMP_END: {
        const act_faultchat_pump_end_t *p = &a->u.faultchat_pump_end;
        f->busy        = 0;
        f->last_result = p->result;
        f->last_seq    = p->seq;
        bstr_copy(f->last_why, p->why, sizeof f->last_why);
        break;
    }

    case ACT_FAULTCHAT_LATCH_OFF:
        f->off     = 1;
        f->off_why = a->u.faultchat_latch_off.why
                     ? a->u.faultchat_latch_off.why
                     : fc_empty;
        break;

    case ACT_FAULTCHAT_DIAGNOSIS_SENT:
        f->diagnoses++;
        f->seen = a->u.faultchat_diagnosis_sent.new_seen;
        break;

    case ACT_FAULTCHAT_FIX_ARMED:
        f->fixes++;
        break;

    case ACT_FAULTCHAT_PATCH_APPLIED:
        f->patches++;
        break;

    case ACT_FAULTCHAT_REPLY_STORED:
        f->last_reply = a->u.faultchat_reply_stored.reply;
        break;

    default:
        break;
    }
}

/* ====================================================================== */
/* Dispatch                                                                */
/* ====================================================================== */

void state_dispatch(const action_t *a)
{
    if (!a) return;
    reduce_chat(&g_state, a);
    reduce_telegram(&g_state, a);
    reduce_patch(&g_state, a);
    reduce_faultchat(&g_state, a);
}
