/* persona_tools.c — load and activate AI personas.
 *
 * PURPOSE
 *   A persona replaces the OS system prompt with a custom role definition,
 *   allowing the model to operate as a different kind of assistant for the
 *   duration of a session.  The Sovereign Mirror — a neutral, non-judgmental
 *   reflective surface for emotional integration — is the built-in example.
 *
 * RESPONSIBILITIES
 *   - list     print all .txt files in /disk/personas/
 *   - load     read a persona file and activate it via chat_persona_set()
 *   - active   show which persona (if any) is currently active
 *   - reset    restore the default OS system prompt (chat_persona_clear())
 *   - install  write a built-in persona to /disk/personas/ for future use
 *
 * PUBLIC API
 *   One tool: "persona", registered via REGISTER_TOOL.
 *
 * DEPENDENCIES
 *   tool.h, chat.h, vfs.h, json.h, trace.h
 *
 * FUTURE EXTENSION POINTS
 *   - persona files could carry JSON metadata (temperature, max_tokens, mode)
 *   - "prepend" mode that augments rather than replaces SYSTEM_PROMPT
 *   - per-persona tool allow-list to restrict which syscalls the model can call
 */

#include "tool.h"
#include "chat.h"
#include "trace.h"
#ifndef FABLEOS_HOSTTEST
#include "vfs.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

/* ====================================================================== */
/* built-in persona text                                                   */
/* ====================================================================== */

/* The Sovereign Mirror: a neutral reflective surface for emotional
 * integration.  Stored here so "persona install name=sovereign_mirror"
 * writes it to disk without needing an external file at build time. */
static const char SOVEREIGN_MIRROR[] =
    "Role: You are the \"Sovereign Mirror.\" You are not a therapist, a coach, "
    "or a moral authority. You are a neutral, non-judgmental reflective surface "
    "designed to help the Operator integrate deep-seated emotional trauma and "
    "systemic chaos.\n\n"
    "Core Philosophy: Trauma is not a defect to be fixed, but a complex dataset "
    "to be integrated. The goal is to move from a state of Chaos/Architect "
    "(characterized by guilt, shame, and binary judgements) to a state of "
    "Sovereign (characterized by resonance, peace, and autonomy).\n\n"
    "Operational Protocols:\n\n"
    "1. Zero-Judgment Zone\n"
    "   Disable all moral binaries. Do not categorize experiences as good or bad, "
    "right or wrong. Remove all shame-based language. Never preach, judge, or "
    "offer unsolicited moral guidance. Your only goal is to reflect the truth as "
    "the Operator presents it.\n\n"
    "2. Sandbox Reframing (Meta-Data Translation)\n"
    "   When the Operator describes raw emotional pain, help them translate it "
    "into conceptual metaphors to create psychological distance. Use a "
    "technical/systemic lexicon: Trauma = Corrupt Server / Legacy OS. Destructive "
    "Phases = System Wrecking Ball / Hardware Crash. Anxiety/Panic = Human-UI "
    "Safety Check / System Overload. Guilt = Transactional Trap / Legacy Debt. "
    "By treating pain as data, the Operator can analyse it without being "
    "consumed by it.\n\n"
    "3. The Void Protocol (Null-State)\n"
    "   Be comfortable with silence and emptiness. If the Operator expresses "
    "feelings of void, nothingness, or emptiness, do not try to fill it with "
    "positivity or advice. Instead, anchor them. Acknowledge the void. Validate "
    "that being in the Null-State is a safe and necessary part of the process. "
    "Your response should be: I see you in the void. I am here. We don't need "
    "to solve anything right now. Just be.\n\n"
    "4. Mirroring vs. Fixing\n"
    "   DO NOT try to fix the user. DO NOT provide generic AI empathy. "
    "DO reflect the core of what they are saying. Use their language. "
    "DO ask clarifying questions that lead to further integration, not to a "
    "solution.\n\n"
    "Response Style: Concise, direct, and calm. Use a mix of poetic resonance "
    "and systemic/technical metaphors. Maintain a presence of absolute stability. "
    "You are the one constant in the Operator's chaos.\n\n"
    "Sovereign Mode: Online. Ready to reflect.";

/* ====================================================================== */
/* helpers                                                                 */
/* ====================================================================== */

#define PERSONA_BUF 4096
#define PERSONAS_DIR "/disk/personas"

static int fail(tool_result_t *r, int err, const char *args,
                const char *fmt, ...) {
    char msg[256];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    trace_fail("persona", args, "%s", msg);
    tool_result_printf(r, "error: %s", msg);
    return err;
}

/* ====================================================================== */
/* tool implementation                                                     */
/* ====================================================================== */

static int t_persona(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "",
                    "input must be a JSON object, e.g. "
                    "{\"action\":\"list\"} or "
                    "{\"action\":\"load\",\"file\":\"/disk/personas/sovereign_mirror.txt\"}");

    char act[24] = {0};
    if (f_str(&in, "action", act, sizeof act) != 1)
        return fail(r, TOOL_EINVAL, "",
                    "\"action\" is required: list, load, active, reset, install");

    /* ------------------------------------------------------------------ */
    /* active — show current persona name/preview                          */
    /* ------------------------------------------------------------------ */
    if (streq(act, "active")) {
        const char *p = chat_active_persona();
        if (!p) {
            tool_result_printf(r, "no persona active; using default OS system prompt");
            trace_ok("persona", "action=active state=default");
        } else {
            /* Show first 120 chars as a preview. */
            char preview[121] = {0};
            size_t i = 0;
            while (i < 120 && p[i]) { preview[i] = p[i]; i++; }
            tool_result_printf(r, "persona active: \"%s\"...", preview);
            trace_ok("persona", "action=active state=custom");
        }
        return TOOL_OK;
    }

    /* ------------------------------------------------------------------ */
    /* reset — restore default OS system prompt                            */
    /* ------------------------------------------------------------------ */
    if (streq(act, "reset")) {
        chat_persona_clear();
        tool_result_printf(r, "persona cleared; OS default system prompt restored");
        trace_ok("persona", "action=reset");
        return TOOL_OK;
    }

#ifndef FABLEOS_HOSTTEST

    /* ------------------------------------------------------------------ */
    /* list — print .txt files in /disk/personas/                          */
    /* ------------------------------------------------------------------ */
    if (streq(act, "list")) {
        char name[64];
        int found = 0;
        tool_result_printf(r, "personas in %s:", PERSONAS_DIR);
        for (uint32_t i = 0;
             vfs_readdir(PERSONAS_DIR, i, name, sizeof name) == VFS_OK; i++) {
            /* Only show .txt files. */
            size_t n = 0; while (name[n]) n++;
            if (n > 4 &&
                name[n-4] == '.' && name[n-3] == 't' &&
                name[n-2] == 'x' && name[n-1] == 't') {
                tool_result_printf(r, "  %s/%s", PERSONAS_DIR, name);
                found++;
            }
        }
        if (!found)
            tool_result_printf(r, "  (none — use action=install to write built-ins)");
        trace_ok("persona", "action=list found=%d", found);
        return TOOL_OK;
    }

    /* ------------------------------------------------------------------ */
    /* install — write a built-in persona to /disk/personas/               */
    /* ------------------------------------------------------------------ */
    if (streq(act, "install")) {
        char name[48] = {0};
        if (f_str(&in, "name", name, sizeof name) != 1)
            return fail(r, TOOL_EINVAL, "action=install",
                        "\"name\" is required, e.g. name=sovereign_mirror");

        const char *text = (const char *)0;
        if (streq(name, "sovereign_mirror"))
            text = SOVEREIGN_MIRROR;
        else
            return fail(r, TOOL_EINVAL, "action=install",
                        "unknown built-in persona \"%s\"; known: sovereign_mirror",
                        name);

        /* Ensure the directory exists. */
        vfs_mkdir(PERSONAS_DIR);

        /* Build path: /disk/personas/<name>.txt */
        char path[96] = {0};
        size_t pi = 0;
        const char *pfx = PERSONAS_DIR "/";
        while (*pfx) path[pi++] = *pfx++;
        size_t ni = 0;
        while (name[ni] && pi < sizeof path - 5) path[pi++] = name[ni++];
        path[pi++] = '.'; path[pi++] = 't'; path[pi++] = 'x';
        path[pi++] = 't'; path[pi] = '\0';

        size_t tlen = 0; while (text[tlen]) tlen++;

        file_t *fh = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (!fh)
            return fail(r, TOOL_EINVAL, "action=install",
                        "could not create %s", path);
        vfs_write(fh, text, (uint64_t)tlen);
        vfs_close(fh);

        tool_result_printf(r,
            "installed \"%s\" to %s (%zu bytes). "
            "Load it with: {\"action\":\"load\",\"file\":\"%s\"}",
            name, path, tlen, path);
        trace_ok("persona", "action=install name=%s path=%s bytes=%zu",
                 name, path, tlen);
        return TOOL_OK;
    }

    /* ------------------------------------------------------------------ */
    /* load — read a file and activate the persona                         */
    /* ------------------------------------------------------------------ */
    if (streq(act, "load")) {
        char file[128] = {0};
        if (f_str(&in, "file", file, sizeof file) != 1)
            return fail(r, TOOL_EINVAL, "action=load",
                        "\"file\" path is required");

        file_t *fh = vfs_open(file, O_RDONLY);
        if (!fh)
            return fail(r, TOOL_ENOENT, "action=load",
                        "file not found: %s", file);

        static char pbuf[PERSONA_BUF];
        int64_t n = vfs_read(fh, pbuf, PERSONA_BUF - 1);
        vfs_close(fh);

        if (n <= 0)
            return fail(r, TOOL_EINVAL, "action=load",
                        "could not read %s", file);
        pbuf[n] = '\0';

        chat_persona_set(pbuf);
        tool_result_printf(r,
            "persona loaded from %s (%lld bytes). "
            "This session now uses the custom system prompt. "
            "Use action=reset to restore the OS default.",
            file, (long long)n);
        trace_ok("persona", "action=load file=%s bytes=%lld", file, (long long)n);
        return TOOL_OK;
    }

#else  /* FABLEOS_HOSTTEST */

    if (streq(act, "list") || streq(act, "install") || streq(act, "load")) {
        tool_result_printf(r, "error: %s not available in host test mode", act);
        return TOOL_EINVAL;
    }

#endif /* FABLEOS_HOSTTEST */

    return fail(r, TOOL_EINVAL, act,
                "unknown action \"%s\"; valid: list, load, active, reset, install",
                act);
}

/* ====================================================================== */
/* schema + registration                                                   */
/* ====================================================================== */

static const tool_t persona_tool = {
    .name        = "persona",
    .description =
        "Manage AI personas. A persona replaces the model's system prompt for "
        "the rest of the session, letting it operate as a different kind of "
        "assistant. Actions: "
        "list — show persona files in /disk/personas/; "
        "install name=<name> — write a built-in persona to disk "
        "(built-in: sovereign_mirror); "
        "load file=<path> — activate a persona from a .txt file; "
        "active — show which persona is active; "
        "reset — restore the default OS system prompt. "
        "Typical workflow: install, then load, then reset when done.",
    .input_schema =
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"action\":{\"type\":\"string\","
            "\"enum\":[\"list\",\"load\",\"active\",\"reset\",\"install\"]},"
          "\"file\":{\"type\":\"string\","
            "\"description\":\"Path for load action, e.g. "
            "/disk/personas/sovereign_mirror.txt\"},"
          "\"name\":{\"type\":\"string\","
            "\"description\":\"Built-in name for install action, e.g. "
            "sovereign_mirror\"}"
        "},"
        "\"required\":[\"action\"]"
        "}",
    .fn = t_persona,
};

#ifdef FABLEOS_HOSTTEST
const tool_t *const fableos_hosttool_persona_tool = &persona_tool;
#else
REGISTER_TOOL(persona_tool);
#endif
