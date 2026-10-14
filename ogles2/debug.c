/*
 * EXT_debug_marker (M7, docs/M7_EXTENSIONS_PLAN.md step 1): markers a
 * program sets for debugging tools. The extension only asks that the calls
 * be accepted; ogles2 writes them into its log while tracing is on (probe T,
 * ENV:OGLES2/Probe), indented by group depth, so a traced frame shows the
 * program's own structure. A pop without a push is ignored, as the
 * extension says; the depth is kept per context, at most 64 groups deep.
 */
#include "ogles2_internal.h"

#define MAX_GROUP_DEPTH 64

/* "marker" `length` characters long (0: NUL-terminated), at the depth. */
static void log_marker(struct OGLES2Context *ctx, const char *what, GLsizei length, const GLchar *marker)
{
    char text[128], pad[65];
    ULONG n = 0, k, indent = ctx->marker_depth < 32 ? ctx->marker_depth : 32;
    if (!(ctx->probe & OGLES2_PROBE_TRACE))
        return;
    for (k = 0; k < indent * 2; k++) /* RawDoFmt has no "%*s" */
        pad[k] = ' ';
    pad[k] = 0;
    if (marker)
        for (k = 0; n < sizeof(text) - 1 && (length > 0 ? k < (ULONG)length : marker[k] != 0); k++)
            text[n++] = marker[k];
    text[n] = 0;
    ogles2_log("marker: %s%s \"%s\"", (ULONG)pad, (ULONG)what, (ULONG)text);
}

void ogles2_glInsertEventMarkerEXT(struct OGLES2Context *ctx, GLsizei length, const GLchar *marker)
{
    if (ogles2_context_valid(ctx))
        log_marker(ctx, "event", length, marker);
}

void ogles2_glPushGroupMarkerEXT(struct OGLES2Context *ctx, GLsizei length, const GLchar *marker)
{
    if (!ogles2_context_valid(ctx))
        return;
    log_marker(ctx, "group", length, marker);
    if (ctx->marker_depth < MAX_GROUP_DEPTH)
        ctx->marker_depth++;
}

void ogles2_glPopGroupMarkerEXT(struct OGLES2Context *ctx)
{
    if (!ogles2_context_valid(ctx) || !ctx->marker_depth)
        return;
    ctx->marker_depth--;
    log_marker(ctx, "end of group", 0, "");
}
