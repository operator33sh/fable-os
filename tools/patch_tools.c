/* patch_tools.c — Runtime symbol lookup and live code patching.
 *
 * PURPOSE
 *   Five tools that let the model find and modify kernel function code at
 *   run time, with a per-function rollback store so every patch is undoable.
 *
 * THE TOOLS
 *   patch_list      Enumerate every symbol in .ksymtab with its address and
 *                   whether it lies inside .text (= actually patchable).
 *
 *   patch_symbol    Look up a single symbol by exact name. Returns address
 *                   and .text membership. Errors if the name is not exported.
 *
 *   patch_apply     Write new instruction bytes to an exported function,
 *                   saving the original bytes in the rollback store first.
 *                   The target must be in [__text_start, __text_end).
 *
 *   patch_rollback  Restore an exported function's original bytes from the
 *                   rollback store. Clears the slot so it can be reused.
 *
 *   patch_status    List every currently active patch: name, address,
 *                   byte count, and hex dump of what was written.
 *
 * ARCHITECTURE
 *   EXPORT_SYMBOL(fn) in any kernel .c file places a { name, address } entry
 *   in .ksymtab (include/ksym.h). This file scans that table for lookups and
 *   for address validation before writing.
 *
 *   All kernel pages are RWX (boot/boot.asm maps 2 MiB huge pages with no NX;
 *   see AGENTS.md). memcpy into .text is legal without any mprotect call. On
 *   x86_64, code-cache coherence is guaranteed by the architecture: the CPU
 *   sees new bytes as soon as they are written, so no serialising instruction
 *   is needed on a single-core cooperative kernel.
 *
 *   Rollback state lives in g_state.patch (core/state.c). patch_slot_t and
 *   the STATE_PATCH_* size constants are defined in include/state.h.
 *   Transitions go through state_dispatch(ACT_PATCH_APPLY/ROLLBACK): the
 *   .text write happens first, the state update follows only on success.
 *
 * SAFETY CONSTRAINTS (enforced here, not in the handler)
 *   1. Target address must be in [__text_start, __text_end).
 *   2. Target address + patch length must not exceed __text_end.
 *   3. Patch length must be 1..STATE_PATCH_MAX_BYTES.
 *   4. The function must be exported (in .ksymtab).
 *   5. No double-patch without an intervening rollback.
 *
 * WHY NOT patch chat_ask WHILE INSIDE chat_ask
 *   A tool call runs on the call stack chat_ask -> chat_ask_body ->
 *   tool_dispatch -> patch_apply. Writing new bytes to chat_ask's entry
 *   point is safe for the CURRENT invocation (the CPU is already past those
 *   bytes) but takes effect on the NEXT call. The model should be aware of
 *   this: a patch to the current function's prologue will not be seen until
 *   the next turn.
 *
 * DEPENDENCIES
 *   state.h (patch_slot_t, STATE_PATCH_*, g_state, state_dispatch),
 *   ksym.h, tool.h, json.h, <string.h>. No fetch, no lwIP, no kernel.h.
 *
 * FUTURE EXTENSION POINTS
 *   patch_trampoline  — install a 5-byte near-JMP to a model-compiled stub
 *   patch_export      — add a new EXPORT_SYMBOL entry dynamically (needs a
 *                       mutable .ksymtab, not a read-only section)
 */

#include "state.h"
#include "ksym.h"
#include "tool.h"
#include "json.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Linker-defined .text bounds from linker.ld. */
extern char __text_start[];
extern char __text_end[];

/* ====================================================================== */
/* Rollback store helpers — read from g_state.patch                       */
/* ====================================================================== */

static patch_slot_t *slot_by_name(const char *name) {
    for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
        patch_slot_t *s = &g_state.patch.slots[i];
        if (s->active &&
            strncmp(s->name, name, STATE_PATCH_NAME_MAX) == 0)
            return s;
    }
    return (patch_slot_t *)0;
}

static patch_slot_t *slot_free(void) {
    for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
        if (!g_state.patch.slots[i].active)
            return &g_state.patch.slots[i];
    }
    return (patch_slot_t *)0;
}

/* ====================================================================== */
/* Hex decoder                                                             */
/* ====================================================================== */

/* Convert a single hex character to its value, or -1 if invalid. */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a hex string (with optional spaces, colons, or tabs as separators)
 * into out[cap].  Sets *outlen to the number of bytes written.
 * Returns 0 on success, -1 on invalid character, -2 on overflow. */
static int hex_decode(const char *s, uint8_t *out, size_t cap, size_t *outlen) {
    size_t      n = 0;
    const char *p = s;
    while (*p) {
        /* skip separators */
        while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n') p++;
        if (!*p) break;
        int hi = hex_nibble(*p++);
        if (hi < 0) return -1;
        /* must have a second nibble */
        int lo = hex_nibble(*p++);
        if (lo < 0) return -1;
        if (n >= cap) return -2;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    *outlen = n;
    return 0;
}

/* Hex-encode src[len] into dst[dsz] (with spaces between bytes).
 * Returns the number of characters written (excluding NUL), or -1 if dst is
 * too small. Each byte is "XX "; the trailing space is overwritten with NUL. */
static int hex_encode(const uint8_t *src, size_t len, char *dst, size_t dsz) {
    if (dsz == 0) return -1;
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        /* "XX " = 3 chars + NUL */
        if (w + 4 > dsz) return -1;
        const char hex[] = "0123456789abcdef";
        dst[w++] = hex[(src[i] >> 4) & 0xf];
        dst[w++] = hex[src[i] & 0xf];
        dst[w++] = ' ';
    }
    if (w > 0 && dst[w - 1] == ' ') w--;   /* trim trailing space */
    dst[w] = '\0';
    return (int)w;
}

/* ====================================================================== */
/* Shared argument helpers                                                 */
/* ====================================================================== */

/* Parse call->input as a JSON object.  Returns 0 on success. */
static int pt_arg_root(const tool_call_t *call, json_value_t *root,
                       char *err, size_t ecap) {
    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, root) != JSON_OK ||
        root->type != JSON_OBJECT) {
        snprintf(err, ecap, "expected a JSON object of arguments");
        return -1;
    }
    return 0;
}

/* Extract the string value of key from root into dst[cap].
 * Returns 0 if the key is absent (dst is left empty), 1 if found, -1 on error. */
static int pt_arg_str(const json_value_t *root, const char *key,
                      char *dst, size_t cap, char *err, size_t ecap) {
    json_value_t v;
    err[0] = '\0';
    if (json_get(root, key, &v) != JSON_OK) { dst[0] = '\0'; return 0; }
    if (v.type != JSON_STRING) {
        snprintf(err, ecap, "\"%s\" must be a string", key);
        return -1;
    }
    size_t n = 0;
    int rc = json_str(&v, dst, cap, &n);
    if (rc == JSON_ENOSPC) {
        snprintf(err, ecap, "\"%s\" exceeds the %u-byte limit",
                 key, (unsigned)cap - 1);
        return -1;
    }
    if (rc != JSON_OK) {
        snprintf(err, ecap, "\"%s\" is not a readable string", key);
        return -1;
    }
    if (strlen(dst) != n) {
        snprintf(err, ecap, "\"%s\" contains an embedded NUL byte", key);
        return -1;
    }
    return 1;
}

/* ====================================================================== */
/* patch_list                                                              */
/* ====================================================================== */

static int t_patch_list(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    size_t n = ksym_count();
    if (n == 0) {
        tool_result_printf(r,
            "no exported symbols — add EXPORT_SYMBOL(fn) after a function "
            "definition in any kernel .c file to make it visible here\n");
        return TOOL_OK;
    }

    tool_result_printf(r, "exported symbols: %zu\n", n);
    for (const ksym_entry_t *e = __start_ksymtab; e < __stop_ksymtab; e++) {
        int in_text = ((char *)e->addr >= __text_start &&
                       (char *)e->addr <  __text_end);
        patch_slot_t *s = slot_by_name(e->name);
        tool_result_printf(r, "  %-40s  0x%016lx  %-14s  %s\n",
                           e->name,
                           (unsigned long)e->addr,
                           in_text ? ".text" : "outside .text",
                           s ? "PATCHED" : "");
    }
    return TOOL_OK;
}

static const tool_t patch_list_tool = {
    .name        = "patch_list",
    .description =
        "List every kernel function exported for runtime patching via "
        "EXPORT_SYMBOL. Shows each function's name, address, and whether "
        "it is currently patched. Use patch_symbol for a single lookup.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_patch_list,
};
REGISTER_TOOL(patch_list_tool);

/* ====================================================================== */
/* patch_symbol                                                            */
/* ====================================================================== */

static int t_patch_symbol(const tool_call_t *call, tool_result_t *r) {
    char         err[160];
    json_value_t root;
    if (pt_arg_root(call, &root, err, sizeof err) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }

    char name[STATE_PATCH_NAME_MAX];
    if (pt_arg_str(&root, "name", name, sizeof name, err, sizeof err) <= 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n",
                           err[0] ? err : "\"name\" (string) is required");
        return TOOL_OK;
    }

    uintptr_t addr = ksym_lookup(name);
    if (!addr) {
        r->is_error = 1;
        tool_result_printf(r,
            "symbol \"%s\" not found — annotate it with EXPORT_SYMBOL in "
            "the kernel source; use patch_list to see what is available\n",
            name);
        return TOOL_OK;
    }

    int in_text = ((char *)addr >= __text_start && (char *)addr < __text_end);
    patch_slot_t *s = slot_by_name(name);

    tool_result_printf(r,
        "name:    %s\n"
        "address: 0x%016lx\n"
        "in_text: %s\n"
        "status:  %s\n",
        name,
        (unsigned long)addr,
        in_text ? "yes" : "no (not inside .text — not patchable)",
        s ? "PATCHED (patch_rollback to restore)" : "unpatched");

    return TOOL_OK;
}

static const tool_t patch_symbol_tool = {
    .name        = "patch_symbol",
    .description =
        "Look up the run-time address of an exported kernel function by name. "
        "Returns address, .text membership, and patch status. "
        "Use patch_list to enumerate available symbols.",
    .input_schema =
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"name\":{"
              "\"type\":\"string\","
              "\"description\":\"Exact function name as exported with EXPORT_SYMBOL\""
            "}"
          "},"
          "\"required\":[\"name\"]"
        "}",
    .flags  = 0,
    .invoke = t_patch_symbol,
};
REGISTER_TOOL(patch_symbol_tool);

/* ====================================================================== */
/* patch_apply                                                             */
/* ====================================================================== */

static char pa_hex_arg[STATE_PATCH_MAX_BYTES * 3 + 1];   /* "XX XX ..." */
static char pa_name_arg[STATE_PATCH_NAME_MAX];

static int t_patch_apply(const tool_call_t *call, tool_result_t *r) {
    char         err[160];
    json_value_t root;
    if (pt_arg_root(call, &root, err, sizeof err) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }

    /* name */
    if (pt_arg_str(&root, "name", pa_name_arg, sizeof pa_name_arg,
                   err, sizeof err) <= 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n",
                           err[0] ? err : "\"name\" (string) is required");
        return TOOL_OK;
    }

    /* hex */
    if (pt_arg_str(&root, "hex", pa_hex_arg, sizeof pa_hex_arg,
                   err, sizeof err) <= 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n",
                           err[0] ? err : "\"hex\" (string) is required");
        return TOOL_OK;
    }

    /* decode hex -> bytes */
    uint8_t bytes[STATE_PATCH_MAX_BYTES];
    size_t  blen = 0;
    int hrc = hex_decode(pa_hex_arg, bytes, sizeof bytes, &blen);
    if (hrc == -1) {
        r->is_error = 1;
        tool_result_printf(r,
            "\"hex\" contains an invalid character; expected pairs of "
            "hex digits optionally separated by spaces (e.g. \"90 90\")\n");
        return TOOL_OK;
    }
    if (hrc == -2 || blen > STATE_PATCH_MAX_BYTES) {
        r->is_error = 1;
        tool_result_printf(r,
            "patch is too long: maximum is %d bytes\n", STATE_PATCH_MAX_BYTES);
        return TOOL_OK;
    }
    if (blen == 0) {
        r->is_error = 1;
        tool_result_printf(r, "\"hex\" must not be empty\n");
        return TOOL_OK;
    }

    /* look up the symbol */
    uintptr_t addr = ksym_lookup(pa_name_arg);
    if (!addr) {
        r->is_error = 1;
        tool_result_printf(r,
            "symbol \"%s\" not found in .ksymtab; use patch_list to see "
            "exported functions\n", pa_name_arg);
        return TOOL_OK;
    }

    /* must be inside .text */
    if ((char *)addr < __text_start || (char *)addr >= __text_end) {
        r->is_error = 1;
        tool_result_printf(r,
            "0x%016lx is not inside .text [0x%016lx, 0x%016lx) — "
            "refusing to patch\n",
            (unsigned long)addr,
            (unsigned long)__text_start,
            (unsigned long)__text_end);
        return TOOL_OK;
    }

    /* patch must not extend past .text */
    if ((char *)addr + blen > __text_end) {
        r->is_error = 1;
        tool_result_printf(r,
            "patch of %zu bytes starting at 0x%016lx would extend past "
            "the end of .text (0x%016lx)\n",
            blen, (unsigned long)addr, (unsigned long)__text_end);
        return TOOL_OK;
    }

    /* refuse double-patch */
    if (slot_by_name(pa_name_arg)) {
        r->is_error = 1;
        tool_result_printf(r,
            "\"%s\" is already patched; call patch_rollback first\n",
            pa_name_arg);
        return TOOL_OK;
    }

    /* refuse if rollback store is full */
    if (!slot_free()) {
        r->is_error = 1;
        tool_result_printf(r,
            "rollback store is full (%d active patches); "
            "call patch_rollback on one before adding another\n",
            STATE_PATCH_MAX_SLOTS);
        return TOOL_OK;
    }

    /* save original bytes before writing */
    uint8_t orig[STATE_PATCH_MAX_BYTES];
    memcpy(orig, (const void *)addr, blen);

    /* write the patch — all pages are RWX (AGENTS.md) */
    memcpy((void *)addr, bytes, blen);

    /* record in g_state AFTER the write succeeds */
    action_t act;
    memset(&act, 0, sizeof act);
    act.type = ACT_PATCH_APPLY;
    strncpy(act.u.patch_apply.name, pa_name_arg,
            sizeof act.u.patch_apply.name - 1);
    act.u.patch_apply.addr = addr;
    act.u.patch_apply.len  = (uint8_t)blen;
    memcpy(act.u.patch_apply.orig,        orig,  blen);
    memcpy(act.u.patch_apply.patch_bytes, bytes, blen);
    state_dispatch(&act);

    char hex_orig[STATE_PATCH_MAX_BYTES * 3 + 1];
    char hex_new [STATE_PATCH_MAX_BYTES * 3 + 1];
    hex_encode(orig,  blen, hex_orig, sizeof hex_orig);
    hex_encode(bytes, blen, hex_new,  sizeof hex_new);

    tool_result_printf(r,
        "patched %s at 0x%016lx (%zu bytes)\n"
        "  original: %s\n"
        "  new:      %s\n"
        "call patch_rollback to restore the original bytes\n",
        pa_name_arg,
        (unsigned long)addr,
        blen,
        hex_orig,
        hex_new);

    return TOOL_OK;
}

static const tool_t patch_apply_tool = {
    .name        = "patch_apply",
    .description =
        "Write new instruction bytes to an exported kernel function. "
        "Saves the original bytes first so patch_rollback can undo the change. "
        "The function must be in .ksymtab (EXPORT_SYMBOL) and inside .text. "
        "Only one live patch per function; call patch_rollback before re-patching. "
        "hex: byte string as hex pairs, optionally space-separated (e.g. \"90 90 c3\"). "
        "Maximum patch size: 64 bytes.",
    .input_schema =
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"name\":{"
              "\"type\":\"string\","
              "\"description\":\"Exact function name as exported with EXPORT_SYMBOL\""
            "},"
            "\"hex\":{"
              "\"type\":\"string\","
              "\"description\":"
                "\"New instruction bytes as hex pairs, e.g. \\\"90 90 c3\\\" or \\\"9090c3\\\"\""
            "}"
          "},"
          "\"required\":[\"name\",\"hex\"]"
        "}",
    .flags  = TOOL_MUTATES,
    .invoke = t_patch_apply,
};
REGISTER_TOOL(patch_apply_tool);

/* ====================================================================== */
/* patch_rollback                                                          */
/* ====================================================================== */

static char pr_name_arg[STATE_PATCH_NAME_MAX];

static int t_patch_rollback(const tool_call_t *call, tool_result_t *r) {
    char         err[160];
    json_value_t root;
    if (pt_arg_root(call, &root, err, sizeof err) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }

    if (pt_arg_str(&root, "name", pr_name_arg, sizeof pr_name_arg,
                   err, sizeof err) <= 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n",
                           err[0] ? err : "\"name\" (string) is required");
        return TOOL_OK;
    }

    patch_slot_t *s = slot_by_name(pr_name_arg);
    if (!s) {
        r->is_error = 1;
        tool_result_printf(r,
            "\"%s\" has no active patch; use patch_status to see what "
            "is currently patched\n", pr_name_arg);
        return TOOL_OK;
    }

    /* read rollback data before modifying state */
    uintptr_t addr = s->addr;
    size_t    len  = s->len;
    uint8_t   orig[STATE_PATCH_MAX_BYTES];
    memcpy(orig, s->orig, len);

    /* restore original bytes */
    memcpy((void *)addr, orig, len);

    /* clear the slot in g_state AFTER the write succeeds */
    action_t act;
    memset(&act, 0, sizeof act);
    act.type = ACT_PATCH_ROLLBACK;
    strncpy(act.u.patch_rollback.name, pr_name_arg,
            sizeof act.u.patch_rollback.name - 1);
    state_dispatch(&act);

    char hex_restored[STATE_PATCH_MAX_BYTES * 3 + 1];
    hex_encode(orig, len, hex_restored, sizeof hex_restored);

    tool_result_printf(r,
        "rolled back %s at 0x%016lx (%zu bytes restored)\n"
        "  restored: %s\n",
        pr_name_arg,
        (unsigned long)addr,
        len,
        hex_restored);

    return TOOL_OK;
}

static const tool_t patch_rollback_tool = {
    .name        = "patch_rollback",
    .description =
        "Restore an exported kernel function to its original bytes by undoing "
        "a previous patch_apply. Clears the rollback slot so the function "
        "can be patched again. Use patch_status to see active patches.",
    .input_schema =
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"name\":{"
              "\"type\":\"string\","
              "\"description\":\"Exact function name whose patch should be undone\""
            "}"
          "},"
          "\"required\":[\"name\"]"
        "}",
    .flags  = TOOL_MUTATES,
    .invoke = t_patch_rollback,
};
REGISTER_TOOL(patch_rollback_tool);

/* ====================================================================== */
/* patch_status                                                            */
/* ====================================================================== */

static int t_patch_status(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    int active = 0;
    for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
        if (g_state.patch.slots[i].active) active++;
    }

    tool_result_printf(r,
        "active patches: %d / %d slots used\n", active, STATE_PATCH_MAX_SLOTS);

    if (active == 0) {
        tool_result_printf(r, "no functions are currently patched\n");
        return TOOL_OK;
    }

    for (int i = 0; i < STATE_PATCH_MAX_SLOTS; i++) {
        const patch_slot_t *s = &g_state.patch.slots[i];
        if (!s->active) continue;

        char hex_orig [STATE_PATCH_MAX_BYTES * 3 + 1];
        char hex_patch[STATE_PATCH_MAX_BYTES * 3 + 1];
        hex_encode(s->orig,        s->len, hex_orig,  sizeof hex_orig);
        hex_encode(s->patch_bytes, s->len, hex_patch, sizeof hex_patch);

        tool_result_printf(r,
            "  %s\n"
            "    address:  0x%016lx\n"
            "    size:     %u bytes\n"
            "    original: %s\n"
            "    patched:  %s\n",
            s->name,
            (unsigned long)s->addr,
            (unsigned)s->len,
            hex_orig,
            hex_patch);
    }
    return TOOL_OK;
}

static const tool_t patch_status_tool = {
    .name        = "patch_status",
    .description =
        "Show all currently active patches: function name, address, byte count, "
        "and hex dumps of the original and patched bytes. "
        "Useful before patch_rollback to confirm what is live.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_patch_status,
};
REGISTER_TOOL(patch_status_tool);
