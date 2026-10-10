/*
 * libogles2.a: the agl* functions that maintain the current context, and the
 * varargs forms (plan sections 6.2 and 6.4). The other agl* functions are
 * generated (agl_gen.c).
 */
#include <exec/types.h>
#include <utility/tagitem.h>
#include <proto/ogles2.h>

void aglMakeCurrent(void *context)
{
    if (!OGLES2Dispatch)
        return;
    OGLES2CurrentContext = (struct OGLES2Context *)context;
    OGLES2Dispatch->aglMakeCurrent((struct OGLES2Context *)context);
}

void aglDestroyContext(void *context)
{
    if (!OGLES2Dispatch || !context)
        return;
    if (OGLES2CurrentContext == (struct OGLES2Context *)context)
        OGLES2CurrentContext = 0;
    OGLES2Dispatch->aglDestroyContext((struct OGLES2Context *)context);
}

/* GCC passes all arguments on the stack in order, so the tags after errcode
 * already form a TagItem array (the usual AmigaOS varargs idiom). */
void *aglCreateContextTags2(ULONG *errcode, ...)
{
    return aglCreateContext2(errcode, (struct TagItem *)(&errcode + 1));
}

void aglSetParamsTags2(ULONG tag1, ...)
{
    aglSetParams2((struct TagItem *)&tag1);
}
