/* patch_tools.c — Runtime symbol lookup for live kernel patching.
 *
 * PURPOSE
 *   Two tools that expose the .ksymtab symbol registry to the model,
 *   providing the prerequisite for live code patching: knowing the run-time
 *   address of a kernel function before writing new bytes there.
 *
 * THE TOOLS
 *   patch_list     Enumerate every symbol in .ksymtab with its address and
 *                  whether it lies inside .text (= actually patchable).
 *
 *   patch_symbol   Look up a single symbol by exact name. Returns address
 *                  and .text membership. Errors if the name is not exported.
 *
 * ARCHITECTURE
 *   EXPORT_SYMBOL(fn) in any kernel .c file places a { name, address } entry
 *   in the .ksymtab linker section (include/ksym.h). This file scans that
 *   table. Only explicitly exported functions appear; nothing is patchable
 *   by accident.
 *
 *   Knowing an address is not sufficient to mutate code. A future
 *   patch_apply tool will:
 *     1. Confirm the target is in [__text_start, __text_end).
 *     2. Save the original bytes to a per-slot rollback store.
 *     3. Write the new bytes (all pages are RWX — see AGENTS.md).
 *     4. Expose patch_rollback to undo the change.
 *
 * SECURITY NOTE
 *   These tools are read-only. They reveal addresses but cannot write
 *   anything. The trust model already allows the model to query fault
 *   frames and PCI config space; function addresses are less sensitive
 *   than fault RIPs. Still: only annotated functions appear here.
 *
 * DEPENDENCIES
 *   ksym.h, tool.h, json.h. No fetch, no lwIP, no kernel.h.
 *
 * FUTURE EXTENSION POINTS
 *   patch_apply name=<fn> hex=<bytes>  — write validated bytes, save rollback
 *   patch_rollback name=<fn>           — restore original bytes
 *   patch_status                       — list live patches and their diffs
 */

#include "ksym.h"
#include "tool.h"
#include "json.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Linker-defined .text bounds from linker.ld.
 * Declared as char[] so the variable's address IS the linker symbol. */
extern char __text_start[];
extern char __text_end[];

/* ====================================================================== */
/* patch_list                                                              */
/* ====================================================================== */

static int t_patch_list(const tool_call_t *call, tool_result_t *r) {
    (void)call;   /* no arguments */

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
        tool_result_printf(r, "  %-40s  0x%016lx  %s\n",
                           e->name,
                           (unsigned long)e->addr,
                           in_text ? ".text" : "outside .text");
    }
    return TOOL_OK;
}

static const tool_t patch_list_tool = {
    .name        = "patch_list",
    .description =
        "List every kernel function that has been exported for runtime symbol "
        "resolution via EXPORT_SYMBOL. Returns each function's name and "
        "run-time address, and whether it lies inside the .text segment "
        "(a prerequisite for live patching). Use patch_symbol for a single "
        "lookup.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_patch_list,
};
REGISTER_TOOL(patch_list_tool);

/* ====================================================================== */
/* patch_symbol                                                            */
/* ====================================================================== */

static int t_patch_symbol(const tool_call_t *call, tool_result_t *r) {
    if (!call || !call->input || call->input_len == 0) {
        r->is_error = 1;
        tool_result_printf(r, "expected a JSON object with \"name\"\n");
        return TOOL_OK;
    }

    json_value_t root;
    if (json_parse(call->input, call->input_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r, "expected a JSON object with \"name\"\n");
        return TOOL_OK;
    }

    json_value_t vname;
    if (json_get(&root, "name", &vname) != JSON_OK ||
        vname.type != JSON_STRING) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" (string) is required\n");
        return TOOL_OK;
    }

    char name[128];
    size_t nlen = 0;
    int rc = json_str(&vname, name, sizeof name, &nlen);
    if (rc == JSON_ENOSPC) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" exceeds the 127-byte limit\n");
        return TOOL_OK;
    }
    if (rc != JSON_OK) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" is not a readable string\n");
        return TOOL_OK;
    }
    /* Embedded NUL would silently truncate the lookup key. */
    if (strlen(name) != nlen) {
        r->is_error = 1;
        tool_result_printf(r, "\"name\" contains an embedded NUL byte\n");
        return TOOL_OK;
    }

    uintptr_t addr = ksym_lookup(name);
    if (!addr) {
        r->is_error = 1;
        tool_result_printf(r,
            "symbol \"%s\" not found — it must be annotated with "
            "EXPORT_SYMBOL in the kernel source; use patch_list to see "
            "what is available\n", name);
        return TOOL_OK;
    }

    int in_text = ((char *)addr >= __text_start &&
                   (char *)addr <  __text_end);

    tool_result_printf(r,
        "name:    %s\n"
        "address: 0x%016lx\n"
        "in_text: %s\n",
        name,
        (unsigned long)addr,
        in_text ? "yes" : "no (not inside .text — not patchable)");

    return TOOL_OK;
}

static const tool_t patch_symbol_tool = {
    .name        = "patch_symbol",
    .description =
        "Look up the run-time address of an exported kernel function by name. "
        "Returns its address and whether it lies inside the .text segment. "
        "The function must have been annotated with EXPORT_SYMBOL in the "
        "kernel source. Use patch_list to enumerate available symbols.",
    .input_schema =
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"name\":{"
              "\"type\":\"string\","
              "\"description\":"
                "\"Exact function name as exported with EXPORT_SYMBOL\""
            "}"
          "},"
          "\"required\":[\"name\"]"
        "}",
    .flags  = 0,
    .invoke = t_patch_symbol,
};
REGISTER_TOOL(patch_symbol_tool);
