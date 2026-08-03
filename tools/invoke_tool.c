/* invoke_tool.c — invoke and invoke_register: named tools from cc programs.
 *
 * PURPOSE
 *   Two tools that promote a compiled, saved cc program into a discoverable
 *   named tool the model can call by name with typed, named parameters.
 *
 * THE TOOLS
 *   invoke_register  name=X desc="..." params="a,b"
 *     Saves /cc/X.meta and updates the discovery panel in invoke's description.
 *     The program must already be compiled and saved (cc_compile with name=X).
 *
 *   invoke  name=X args={"a":1,"b":2}
 *     Reads /cc/X.meta, maps args object to a positional uint64_t array in
 *     param declaration order, and delegates to cc_call().
 *
 * PANEL
 *   invoke's description ends with a FIXED INVOKE_PANEL_CHARS-byte slot that
 *   lists registered tools — same trick as cc_call and capability_call.
 *   invoke_bringup() populates it at boot; invoke_register() refreshes it.
 *
 * META FORMAT
 *   /cc/<name>.meta is a JSON object:
 *     {"desc":"short description","params":"a,b,c"}
 *   "params" is a comma-separated ordered list of argument names matching the
 *   cc program's declared parameter count.
 *
 * DEPENDENCIES
 *   cc.h, tool.h, json.h, vfs.h, trace.h, kernel.h
 *
 * FUTURE EXTENSION POINTS
 *   invoke_unregister   Remove a .meta file and refresh the panel.
 *   invoke_list         Enumerate registered tools with full descriptions.
 *   Float/string params If cc gains non-integer args, extend the meta type field.
 */

#include "cc.h"
#include "tool.h"
#include "json.h"
#include "vfs.h"
#include "trace.h"
#include "kernel.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define INVOKE_ERR_MAX     160
#define INVOKE_MAX_PARAMS  CC_MAX_PARAMS               /* 6 — matches cc programs */
#define INVOKE_DESC_MAX    160
#define INVOKE_PARAMS_MAX  (INVOKE_MAX_PARAMS * CC_PARAM_NAME_MAX)
#define META_MAX           512
#define META_DIR           "/cc"
#define META_SUFFIX        ".meta"

/* ====================================================================== */
/* Panel — fixed INVOKE_PANEL_CHARS bytes embedded in g_invoke_desc       */
/* ====================================================================== */

#define INVOKE_PANEL_CHARS 128

/* Exactly 128 spaces; pinned by _Static_assert below. */
#define IP64  "                                                                "
#define IP128 IP64 IP64

_Static_assert(sizeof(IP128) - 1 == INVOKE_PANEL_CHARS,
               "panel padding must be exactly INVOKE_PANEL_CHARS characters, "
               "or the schema changes size and CHAT_REGISTRY_BYTES goes stale");

#define INVOKE_HEAD \
    "Call a user-registered tool by name. Use invoke_register first. REGISTERED: "

static char g_invoke_desc[] = INVOKE_HEAD IP128;
#define INVOKE_PANEL_OFF (sizeof(INVOKE_HEAD) - 1)

_Static_assert(sizeof g_invoke_desc == INVOKE_PANEL_OFF + INVOKE_PANEL_CHARS + 1,
               "g_invoke_desc must be exactly prose + panel + NUL");

/* ====================================================================== */
/* meta helpers                                                            */
/* ====================================================================== */

static int meta_path(const char *name, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "%s/%s%s", META_DIR, name, META_SUFFIX);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Parse a comma-separated params string into an array of names.
 * Returns the number of params (0..INVOKE_MAX_PARAMS), or -1 on error. */
static int split_params(const char *s,
                        char names[INVOKE_MAX_PARAMS][CC_PARAM_NAME_MAX],
                        char *err, size_t ecap) {
    if (!s || !s[0]) return 0;
    int n = 0;
    const char *p = s;
    while (*p) {
        if (n == INVOKE_MAX_PARAMS) {
            snprintf(err, ecap, "too many params (max %d)", INVOKE_MAX_PARAMS);
            return -1;
        }
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 0 || len >= (size_t)CC_PARAM_NAME_MAX) {
            snprintf(err, ecap, "param at position %d is empty or too long", n);
            return -1;
        }
        memcpy(names[n], p, len);
        names[n][len] = '\0';
        n++;
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

/* Write s as a quoted, escaped JSON string into buf[cap].
 * Returns bytes written (including quotes), or -1 on overflow. */
static int json_str_encode(char *buf, size_t cap, const char *s) {
    size_t w = 0;
#define EMIT(c) do { if (w + 1 >= cap) return -1; buf[w++] = (c); } while (0)
    EMIT('"');
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  { EMIT('\\'); EMIT('"');  }
        else if (c == '\\') { EMIT('\\'); EMIT('\\'); }
        else if (c == '\n') { EMIT('\\'); EMIT('n');  }
        else if (c == '\r') { EMIT('\\'); EMIT('r');  }
        else if (c == '\t') { EMIT('\\'); EMIT('t');  }
        else if (c < 0x20)  { /* skip other control bytes */          }
        else                { EMIT(c); }
    }
    EMIT('"');
    buf[w] = '\0';
#undef EMIT
    return (int)w;
}

/* ====================================================================== */
/* panel rendering — must write exactly INVOKE_PANEL_CHARS bytes          */
/* ====================================================================== */

static void render_panel(void) {
    char *slot = g_invoke_desc + INVOKE_PANEL_OFF;
    memset(slot, ' ', INVOKE_PANEL_CHARS);
    /* The NUL at slot[INVOKE_PANEL_CHARS] comes from the string literal; leave it. */

    static char meta_buf[META_MAX];
    char ename[VFS_NAME_MAX + 1];
    size_t used = 0;
    int sep = 0;

    for (uint32_t i = 0; ; i++) {
        if (vfs_readdir(META_DIR, i, ename, sizeof ename) != 0) break;

        size_t elen = strlen(ename);
        size_t slen = sizeof(META_SUFFIX) - 1;
        if (elen <= slen || strcmp(ename + elen - slen, META_SUFFIX) != 0)
            continue;

        /* strip suffix to recover program name */
        char prog[CC_NAME_MAX];
        size_t plen = elen - slen;
        if (plen == 0 || plen >= sizeof prog) continue;
        memcpy(prog, ename, plen);
        prog[plen] = '\0';

        /* read desc from meta */
        char mpath[VFS_PATH_MAX + 1];
        if (meta_path(prog, mpath, sizeof mpath) != 0) continue;
        file_t *fh = vfs_open(mpath, O_RDONLY);
        if (!fh) continue;
        int64_t n = vfs_read(fh, meta_buf, (uint64_t)(META_MAX - 1));
        vfs_close(fh);
        if (n <= 0) continue;
        meta_buf[n] = '\0';

        json_value_t root;
        char desc[64] = "";
        if (json_parse(meta_buf, (size_t)n, &root) == JSON_OK &&
            root.type == JSON_OBJECT)
            json_get_str(&root, "desc", desc, sizeof desc);

        size_t rem = INVOKE_PANEL_CHARS - used;
        int w = snprintf(slot + used, rem + 1,
                         "%s%s%s%s",
                         sep ? " | " : "",
                         prog,
                         desc[0] ? ": " : "",
                         desc);
        if (w <= 0 || (size_t)w > rem) break;
        used += (size_t)w;
        sep = 1;
    }

    if (!sep) {
        const char *none = "(none yet — register with invoke_register after cc_compile)";
        size_t nlen = strlen(none);
        if (nlen > INVOKE_PANEL_CHARS) nlen = INVOKE_PANEL_CHARS;
        memcpy(slot, none, nlen);
        used = nlen;
    }

    /* pad the remainder with spaces to hold the fixed width */
    if (used < INVOKE_PANEL_CHARS)
        memset(slot + used, ' ', INVOKE_PANEL_CHARS - used);
}

/* ====================================================================== */
/* invoke_bringup                                                          */
/* ====================================================================== */

void invoke_bringup(void) {
    render_panel();
}

/* ====================================================================== */
/* invoke_register                                                         */
/* ====================================================================== */

static char reg_meta_json[META_MAX];

static int t_invoke_register(const tool_call_t *call, tool_result_t *r) {
    char err[INVOKE_ERR_MAX];
    json_value_t in;

    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, &in) != JSON_OK ||
        in.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r, "expected a JSON object of arguments\n");
        return TOOL_OK;
    }

    /* name: must match a saved cc program */
    char name[CC_NAME_MAX];
    if (json_get_str(&in, "name", name, sizeof name) != JSON_OK) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" is required\n");
        return TOOL_OK;
    }
    if (!cc_name_ok(name, err, sizeof err)) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\": %s\n", err);
        return TOOL_OK;
    }
    const cc_prog_t *p = cc_find(name);
    if (!p) {
        r->is_error = 1;
        tool_result_printf(r, "no saved program named \"%s\" — "
                              "compile and save it with cc_compile first\n", name);
        return TOOL_OK;
    }

    /* desc: optional, used in the panel */
    char desc[INVOKE_DESC_MAX] = "";
    json_get_str(&in, "desc", desc, sizeof desc);

    /* params: comma-separated ordered argument names, one per cc main() param */
    char params_str[INVOKE_PARAMS_MAX] = "";
    json_get_str(&in, "params", params_str, sizeof params_str);

    char pnames[INVOKE_MAX_PARAMS][CC_PARAM_NAME_MAX];
    memset(pnames, 0, sizeof pnames);
    int np = split_params(params_str, pnames, err, sizeof err);
    if (np < 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }
    if (np != (int)p->nparam) {
        r->is_error = 1;
        tool_result_printf(r, "\"%s\" has %d parameter(s) but params listed %d\n",
                           name, (int)p->nparam, np);
        return TOOL_OK;
    }

    /* build meta JSON: {"desc":<encoded>,"params":"a,b,c"} */
    char desc_json[sizeof(desc) * 2 + 4];
    if (json_str_encode(desc_json, sizeof desc_json, desc) < 0) {
        r->is_error = 1;
        tool_result_printf(r, "desc too long to encode\n");
        return TOOL_OK;
    }
    int mlen = snprintf(reg_meta_json, sizeof reg_meta_json,
                        "{\"desc\":%s,\"params\":\"%s\"}", desc_json, params_str);
    if (mlen <= 0 || (size_t)mlen >= sizeof reg_meta_json) {
        r->is_error = 1;
        tool_result_printf(r, "meta too long to serialize\n");
        return TOOL_OK;
    }

    /* write to /cc/<name>.meta */
    char mpath[VFS_PATH_MAX + 1];
    if (meta_path(name, mpath, sizeof mpath) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "path too long\n");
        return TOOL_OK;
    }
    file_t *fh = vfs_open(mpath, O_WRONLY | O_CREAT);
    if (!fh) {
        r->is_error = 1;
        tool_result_printf(r, "could not create %s\n", mpath);
        return TOOL_OK;
    }
    int64_t written = vfs_write(fh, reg_meta_json, (uint64_t)mlen);
    vfs_close(fh);
    if (written != (int64_t)mlen) {
        r->is_error = 1;
        tool_result_printf(r, "write failed: %ld of %d bytes\n",
                           (long)written, mlen);
        return TOOL_OK;
    }

    render_panel();
    tool_result_printf(r, "registered \"%s\" with %d param(s): %s\n",
                       name, np, np ? params_str : "(none)");
    return TOOL_OK;
}

static const tool_t invoke_register_def = {
    .name        = "invoke_register",
    .description =
        "Register a saved cc program as a callable tool. Must cc_compile+save "
        "it first. Params are comma-separated arg names matching the cc "
        "main() declaration.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"name\":{\"type\":\"string\"},"
          "\"desc\":{\"type\":\"string\"},"
          "\"params\":{\"type\":\"string\"}"
        "},"
        "\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_invoke_register,
};
REGISTER_TOOL(invoke_register_def);

/* ====================================================================== */
/* invoke                                                                  */
/* ====================================================================== */

static cc_result_t g_invoke_res;

static int t_invoke(const tool_call_t *call, tool_result_t *r) {
    char err[INVOKE_ERR_MAX];
    json_value_t in;

    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, &in) != JSON_OK ||
        in.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r, "expected a JSON object of arguments\n");
        return TOOL_OK;
    }

    /* name */
    char name[CC_NAME_MAX];
    if (json_get_str(&in, "name", name, sizeof name) != JSON_OK) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" is required\n");
        return TOOL_OK;
    }
    if (!cc_name_ok(name, err, sizeof err)) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\": %s\n", err);
        return TOOL_OK;
    }
    const cc_prog_t *p = cc_find(name);
    if (!p) {
        r->is_error = 1;
        tool_result_printf(r, "no saved program \"%s\" — "
                              "compile and save it, then invoke_register it\n", name);
        return TOOL_OK;
    }

    /* read meta for param names */
    char mpath[VFS_PATH_MAX + 1];
    if (meta_path(name, mpath, sizeof mpath) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "path too long\n");
        return TOOL_OK;
    }
    static char meta_buf[META_MAX];
    file_t *fh = vfs_open(mpath, O_RDONLY);
    if (!fh) {
        r->is_error = 1;
        tool_result_printf(r, "\"%s\" is not registered — "
                              "run invoke_register first\n", name);
        return TOOL_OK;
    }
    int64_t n = vfs_read(fh, meta_buf, (uint64_t)(META_MAX - 1));
    vfs_close(fh);
    if (n <= 0) {
        r->is_error = 1;
        tool_result_printf(r, "could not read meta for \"%s\"\n", name);
        return TOOL_OK;
    }
    meta_buf[n] = '\0';

    json_value_t root;
    char params_str[INVOKE_PARAMS_MAX] = "";
    if (json_parse(meta_buf, (size_t)n, &root) == JSON_OK &&
        root.type == JSON_OBJECT)
        json_get_str(&root, "params", params_str, sizeof params_str);

    char pnames[INVOKE_MAX_PARAMS][CC_PARAM_NAME_MAX];
    memset(pnames, 0, sizeof pnames);
    int np = split_params(params_str, pnames, err, sizeof err);
    if (np < 0) {
        r->is_error = 1;
        tool_result_printf(r, "meta for \"%s\" has bad params: %s\n", name, err);
        return TOOL_OK;
    }

    /* extract args from the args object, in param declaration order */
    uint64_t args[INVOKE_MAX_PARAMS] = {0};
    if (np > 0) {
        json_value_t vargs;
        if (json_get(&in, "args", &vargs) != JSON_OK ||
            vargs.type != JSON_OBJECT) {
            r->is_error = 1;
            tool_result_printf(r, "\"args\" must be an object with keys: %s\n",
                               params_str);
            return TOOL_OK;
        }
        for (int i = 0; i < np; i++) {
            json_value_t vv;
            if (json_get(&vargs, pnames[i], &vv) != JSON_OK) {
                r->is_error = 1;
                tool_result_printf(r, "missing arg \"%s\"\n", pnames[i]);
                return TOOL_OK;
            }
            int64_t iv = 0;
            if (vv.type != JSON_NUMBER || json_int(&vv, &iv) != JSON_OK) {
                r->is_error = 1;
                tool_result_printf(r, "arg \"%s\" must be an integer\n", pnames[i]);
                return TOOL_OK;
            }
            args[i] = (uint64_t)iv;
        }
    }

    int rc = cc_call(name, args, np, 0, &g_invoke_res);
    if (rc == CC_OK) {
        trace_ret((long)(int64_t)g_invoke_res.value, "invoke", "%s", name);
        if (g_invoke_res.out_len)
            tool_result_printf(r, "%s%s", g_invoke_res.out,
                               g_invoke_res.out[g_invoke_res.out_len - 1] == '\n'
                               ? "" : "\n");
        else
            tool_result_printf(r, "returned %lld\n",
                               (long long)(int64_t)g_invoke_res.value);
        return TOOL_OK;
    }
    trace_err(rc, "invoke", "%s", name);
    r->is_error = 1;
    if (rc == CC_EFUEL) {
        tool_result_printf(r, "STOPPED: fuel exhausted");
        if (g_invoke_res.out_len)
            tool_result_printf(r, "\nbefore stopping it printed:\n%s",
                               g_invoke_res.out);
        return TOOL_OK;
    }
    tool_result_printf(r, "\"%s\" failed (code %d)\n", name, rc);
    return TOOL_OK;
}

static const tool_t invoke_def = {
    .name        = "invoke",
    .description = g_invoke_desc,
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"name\":{\"type\":\"string\"},"
          "\"args\":{\"type\":\"object\"}"
        "},"
        "\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_invoke,
};
REGISTER_TOOL(invoke_def);
