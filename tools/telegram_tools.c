/* telegram_tools.c — Telegram bot inbox and outbox for FableOS.
 *
 * PURPOSE
 *   Two tools that let the model receive and send Telegram messages through
 *   the CloudSiphon bridge running on the QEMU host (10.0.2.2).
 *
 * THE TOOLS
 *   telegram_poll   Pop the oldest pending message from the bridge's queue.
 *                   Returns the sender's name, their chat_id, and the text —
 *                   or "no pending messages" when the queue is empty.
 *
 *   telegram_send   Post a reply to a chat_id. The model provides the chat_id
 *                   it received from telegram_poll and the text to send.
 *
 * ARCHITECTURE
 *   Both tools call fetch() to the CloudSiphon bridge at https://10.0.2.2:
 *     GET  /v1/telegram/poll        — read one queued message
 *     POST /v1/telegram/send        — deliver one message
 *
 *   The bridge runs a background Python thread that long-polls the Telegram
 *   Bot API (getUpdates, timeout=30 s) and queues incoming messages. The
 *   kernel never contacts Telegram directly.
 *
 * CONCURRENCY SAFETY
 *   fetch() uses a single global lwIP backend (net/net.c). FableOS's
 *   cooperative scheduler and the agenda system guarantee these tools only
 *   run between LLM turns, when in_turn == 0 and no other fiber holds the
 *   network. No lock needed beyond what fetch.c already provides.
 *
 * DEPENDENCIES
 *   fetch.h, tool.h, json.h. No kernel.h, no lwIP — so a host test could
 *   link this file against a synthetic fetch backend.
 *
 * FUTURE EXTENSION POINTS
 *   telegram_get_chat_info  — resolve a username to a chat_id
 *   telegram_send_photo     — POST a file (needs multipart/form-data in fetch)
 *   TELEGRAM_ALLOWED_CHATS  — build-time allowlist; currently enforced in the
 *                             bridge (cloud_siphon.py TELEGRAM_ALLOWED_CHAT_IDS)
 */

#include "tool.h"
#include "json.h"
#include "fetch.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* The CloudSiphon bridge on the QEMU host. */
#define BRIDGE_POLL  "https://10.0.2.2/v1/telegram/poll"
#define BRIDGE_SEND  "https://10.0.2.2/v1/telegram/send"

/* Forward declaration — defined in the "JSON string encoder" section below. */
static int json_string_encode(char *buf, size_t cap, const char *s);

/* ====================================================================== */
/* per-turn sent flag                                                      */
/* ====================================================================== */

/* Set to 1 when telegram_send is called during a turn.
 * main.c reads tg_was_sent() after chat_ask() to decide whether to
 * auto-dispatch the model's text response to Telegram. */
static int tg_sent;

void tg_reset_sent(void) { tg_sent = 0; }
int  tg_was_sent(void)   { return tg_sent; }

/* ====================================================================== */
/* auto-reply — called by main.c when the LLM did not call telegram_send  */
/* ====================================================================== */

/* Shared with tg_send_body below. One call at a time, always outside a turn. */
static char tg_auto_body[FETCH_BODY_MAX + 1];

int tg_auto_reply(int64_t chat_id, const char *text) {
    if (!text || !text[0]) return -1;

    char text_json[2048];
    if (json_string_encode(text_json, sizeof text_json, text) < 0) return -1;

    int blen = snprintf(tg_auto_body, sizeof tg_auto_body,
                        "{\"chat_id\":%ld,\"text\":%s}",
                        (long)chat_id, text_json);
    if (blen <= 0 || (size_t)blen >= sizeof tg_auto_body) return -1;

    fetch_options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.method       = "POST";
    opt.body         = tg_auto_body;
    opt.body_len     = (size_t)blen;
    opt.content_type = "application/json";
    opt.timeout_ms   = 10000;

    static char auto_rx[512];
    fetch_result_t fr;
    int rc = fetch(BRIDGE_SEND, sizeof BRIDGE_SEND - 1,
                   &opt, auto_rx, sizeof auto_rx, &fr, (const char **)0);
    return (rc == FETCH_OK && fr.http_status == 200) ? 0 : -1;
}

/* ====================================================================== */
/* static receive buffer — never on the stack (kernel stack is 64 KiB)    */
/* ====================================================================== */

static char tg_rx[4096];

/* ====================================================================== */
/* minimal argument helpers (same pattern as net_tools.c)                 */
/* ====================================================================== */

static int tg_arg_root(const tool_call_t *call, json_value_t *root,
                       char *err, size_t ecap) {
    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, root) != JSON_OK ||
        root->type != JSON_OBJECT) {
        snprintf(err, ecap, "expected a JSON object of arguments");
        return -1;
    }
    return 0;
}

static int tg_arg_str(const json_value_t *root, const char *key,
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
        snprintf(err, ecap, "\"%s\" exceeds the %u-byte limit", key,
                 (unsigned)cap - 1);
        return -1;
    }
    if (rc != JSON_OK) {
        snprintf(err, ecap, "\"%s\" is not a readable string", key);
        return -1;
    }
    /* Reject embedded NUL — would silently truncate the body. */
    if (strlen(dst) != n) {
        snprintf(err, ecap, "\"%s\" contains an embedded NUL byte", key);
        return -1;
    }
    return 1;
}

static int tg_arg_int(const json_value_t *root, const char *key,
                      int64_t lo, int64_t hi, int64_t *out,
                      char *err, size_t ecap) {
    json_value_t v;
    *out = 0;
    if (json_get(root, key, &v) != JSON_OK) {
        snprintf(err, ecap, "\"%s\" is required", key);
        return -1;
    }
    if (v.type != JSON_NUMBER || json_int(&v, out) != JSON_OK) {
        snprintf(err, ecap, "\"%s\" must be an integer", key);
        return -1;
    }
    if (*out < lo || *out > hi) {
        snprintf(err, ecap, "\"%s\" (%ld) is outside %ld..%ld",
                 key, (long)*out, (long)lo, (long)hi);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* JSON string encoder for request body construction                       */
/* ====================================================================== */

/* Write s as a quoted, escaped JSON string into buf[cap].
 * Returns the number of bytes written (including quotes), or -1 on overflow. */
static int json_string_encode(char *buf, size_t cap, const char *s) {
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
/* telegram_poll                                                           */
/* ====================================================================== */

static int t_telegram_poll(const tool_call_t *call, tool_result_t *r) {
    (void)call;   /* no arguments */

    fetch_result_t fr;
    const char    *why = (const char *)0;
    int rc = fetch(BRIDGE_POLL, sizeof BRIDGE_POLL - 1,
                   (const fetch_options_t *)0,
                   tg_rx, sizeof tg_rx, &fr, &why);

    if (rc != FETCH_OK) {
        r->is_error = 1;
        tool_result_printf(r, "could not reach bridge: %s\n",
                           why ? why : fetch_strerror(rc));
        return TOOL_OK;
    }
    if (fr.http_status != 200) {
        r->is_error = 1;
        tool_result_printf(r, "bridge returned HTTP %d\n", fr.http_status);
        return TOOL_OK;
    }
    if (!fr.body || fr.body_len == 0) {
        tool_result_printf(r, "no pending messages\n");
        return TOOL_OK;
    }

    /* Parse the bridge response. Expected:
     *   {"pending":false}
     *   {"pending":true,"chat_id":<n>,"from":"<name>","text":"<msg>"}
     */
    json_value_t root;
    if (json_parse(fr.body, fr.body_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r, "bridge response is not valid JSON\n");
        return TOOL_OK;
    }

    json_value_t vpending;
    if (json_get(&root, "pending", &vpending) != JSON_OK ||
        vpending.type != JSON_BOOL) {
        r->is_error = 1;
        tool_result_printf(r, "bridge response missing \"pending\" field\n");
        return TOOL_OK;
    }

    int pending = 0;
    json_bool(&vpending, &pending);
    if (!pending) {
        tool_result_printf(r, "no pending messages\n");
        return TOOL_OK;
    }

    /* Extract the message fields. */
    json_value_t vchat, vfrom, vtext;
    int64_t chat_id = 0;
    char from_name[128] = {0};
    char text_buf[1024]  = {0};

    if (json_get(&root, "chat_id", &vchat) == JSON_OK &&
        vchat.type == JSON_NUMBER) {
        json_int(&vchat, &chat_id);
    }
    if (json_get(&root, "from", &vfrom) == JSON_OK &&
        vfrom.type == JSON_STRING) {
        size_t n = 0;
        json_str(&vfrom, from_name, sizeof from_name, &n);
    }
    if (json_get(&root, "text", &vtext) == JSON_OK &&
        vtext.type == JSON_STRING) {
        size_t n = 0;
        json_str(&vtext, text_buf, sizeof text_buf, &n);
    }

    tool_result_printf(r,
        "pending: true\n"
        "chat_id: %ld\n"
        "from:    %s\n"
        "text:    %s\n",
        (long)chat_id,
        from_name[0] ? from_name : "(unknown)",
        text_buf);

    return TOOL_OK;
}

static const tool_t telegram_poll_tool = {
    .name        = "telegram_poll",
    .description =
        "Pop the oldest pending Telegram message from the bridge queue. "
        "Returns the sender's name, their chat_id and the message text, "
        "or 'no pending messages' when the queue is empty. "
        "Pass the chat_id to telegram_send when replying.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_telegram_poll,
};
REGISTER_TOOL(telegram_poll_tool);

/* ====================================================================== */
/* telegram_send                                                           */
/* ====================================================================== */

static char tg_send_body[FETCH_BODY_MAX + 1];
static char tg_text_arg[1024];

static int t_telegram_send(const tool_call_t *call, tool_result_t *r) {
    char        err[160];
    json_value_t root;
    if (tg_arg_root(call, &root, err, sizeof err) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }

    int64_t chat_id = 0;
    if (tg_arg_int(&root, "chat_id",
                   -999999999999LL, 999999999999LL,
                   &chat_id, err, sizeof err) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }

    if (tg_arg_str(&root, "text", tg_text_arg, sizeof tg_text_arg,
                   err, sizeof err) < 0) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", err);
        return TOOL_OK;
    }
    if (!tg_text_arg[0]) {
        r->is_error = 1;
        tool_result_printf(r, "\"text\" is required and must not be empty\n");
        return TOOL_OK;
    }

    /* Build the JSON body: {"chat_id":<n>,"text":"<escaped>"} */
    char text_json[sizeof tg_text_arg * 2 + 4];   /* worst-case escaping */
    if (json_string_encode(text_json, sizeof text_json, tg_text_arg) < 0) {
        r->is_error = 1;
        tool_result_printf(r, "text too long to encode\n");
        return TOOL_OK;
    }
    int blen = snprintf(tg_send_body, sizeof tg_send_body,
                        "{\"chat_id\":%ld,\"text\":%s}",
                        (long)chat_id, text_json);
    if (blen < 0 || (size_t)blen >= sizeof tg_send_body) {
        r->is_error = 1;
        tool_result_printf(r, "request body overflow\n");
        return TOOL_OK;
    }

    fetch_options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.method       = "POST";
    opt.body         = tg_send_body;
    opt.body_len     = (size_t)blen;
    opt.content_type = "application/json";
    opt.timeout_ms   = 10000;

    fetch_result_t fr;
    const char    *why = (const char *)0;
    int rc = fetch(BRIDGE_SEND, sizeof BRIDGE_SEND - 1,
                   &opt, tg_rx, sizeof tg_rx, &fr, &why);

    if (rc != FETCH_OK) {
        r->is_error = 1;
        tool_result_printf(r, "could not reach bridge: %s\n",
                           why ? why : fetch_strerror(rc));
        return TOOL_OK;
    }
    if (fr.http_status != 200) {
        r->is_error = 1;
        tool_result_printf(r, "bridge returned HTTP %d\n", fr.http_status);
        return TOOL_OK;
    }

    tg_sent = 1;
    tool_result_printf(r, "sent to chat_id %ld\n", (long)chat_id);
    return TOOL_OK;
}

static const tool_t telegram_send_tool = {
    .name        = "telegram_send",
    .description =
        "Send a Telegram message to a specific chat. "
        "Use the chat_id returned by telegram_poll. "
        "The text may be up to ~1 KB; longer messages should be split.",
    .input_schema =
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"chat_id\":{"
              "\"type\":\"integer\","
              "\"description\":\"Telegram chat ID, as returned by telegram_poll\""
            "},"
            "\"text\":{"
              "\"type\":\"string\","
              "\"description\":\"Message text to send (plain text, max ~1 KB)\""
            "}"
          "},"
          "\"required\":[\"chat_id\",\"text\"]"
        "}",
    .flags  = TOOL_MUTATES,
    .invoke = t_telegram_send,
};
REGISTER_TOOL(telegram_send_tool);
