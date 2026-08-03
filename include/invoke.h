/* invoke.h — invoke_bringup() declaration for kernel/main.c.
 *
 * PURPOSE
 *   Lets main.c call invoke_bringup() without depending on invoke_tool.c's
 *   internals.
 *
 * PUBLIC API
 *   invoke_bringup()  Scan /cc/<name>.meta entries and populate the invoke
 *                     tool's discovery panel.  Call after cc_bringup().
 *
 * DEPENDENCIES
 *   VFS must be mounted.  cc_bringup() must have run.
 *
 * FUTURE EXTENSION POINTS
 *   invoke_panel()    Return the current panel string for diagnostics.
 */

#pragma once

/* Populate the invoke tool's live discovery panel from /cc/<name>.meta files.
 * Must be called after cc_bringup() and VFS mount. */
void invoke_bringup(void);
