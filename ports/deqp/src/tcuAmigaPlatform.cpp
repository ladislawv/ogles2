/*
 * dEQP-GLES2 on AmigaOS 3 (ports/deqp): the platform. A render context is
 * an ogles2 context with its window on the Workbench screen (whatever the
 * surface type asked for: ogles2 has no pbuffers), RGBA8 with depth 24 and
 * stencil 8, 4x multisampled when the configuration asks for samples
 * (--deqp-gl-config-name=rgba8888d24s8ms4), no vsync. The GL functions,
 * core and extensions, are aglGetProcAddress's thunks, which call the
 * current context through the library's dispatch table. Ctrl-C stops the
 * run between tests.
 */
#include "tcuPlatform.hpp"
#include "tcuDefs.hpp"
#include "tcuRenderTarget.hpp"
#include "tcuCommandLine.hpp"
#include "gluPlatform.hpp"
#include "gluContextFactory.hpp"
#include "gluRenderContext.hpp"
#include "gluRenderConfig.hpp"
#include "glwFunctions.hpp"
#include "glwFunctionLoader.hpp"
#include "glwInitFunctions.hpp"
#include "glwEnums.hpp"
#include "deStringUtil.hpp"

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <libraries/ogles2.h>

/* libogles2.a (built by the library's own toolchain; plain C calls). */
extern "C" {
BOOL OGLES2Open(void);
void OGLES2Close(void);
void *aglCreateContextTags2(ULONG *errcode, ...);
void aglSetParamsTags2(ULONG tag1, ...);
void aglDestroyContext(void *context);
void aglMakeCurrent(void *context);
void aglSwapBuffers(void);
void *aglGetProcAddress(const char *name);
}

namespace tcu
{
namespace agl
{

class FunctionLoader : public glw::FunctionLoader
{
public:
    glw::GenericFuncType get(const char *name) const override
    {
        return (glw::GenericFuncType)aglGetProcAddress(name);
    }
};

class RenderContext : public glu::RenderContext
{
public:
    RenderContext(const glu::RenderConfig &config);
    ~RenderContext(void) override;

    glu::ContextType getType(void) const override
    {
        return m_type;
    }
    const glw::Functions &getFunctions(void) const override
    {
        return m_gl;
    }
    const tcu::RenderTarget &getRenderTarget(void) const override
    {
        return m_target;
    }
    void postIterate(void) override
    {
        aglSwapBuffers();
    }
    glw::GenericFuncType getProcAddress(const char *name) const override
    {
        return (glw::GenericFuncType)aglGetProcAddress(name);
    }
    void makeCurrent(void) override
    {
        aglMakeCurrent(m_ctx);
    }

private:
    glu::ContextType m_type;
    void *m_ctx;
    glw::Functions m_gl;
    tcu::RenderTarget m_target;
};

static bool fits(int asked, int have)
{
    return asked == glu::RenderConfig::DONT_CARE || asked == have;
}

RenderContext::RenderContext(const glu::RenderConfig &config) : m_type(config.type), m_ctx(nullptr)
{
    if (config.type.getAPI() != glu::ApiType::es(2, 0))
        throw tcu::NotSupportedError("ogles2.library: OpenGL ES 2.0 only");
    if (config.componentType == glu::RenderConfig::COMPONENT_TYPE_FLOAT)
        throw tcu::NotSupportedError("ogles2.library: no floating-point default framebuffer");
    if (!fits(config.redBits, 8) || !fits(config.greenBits, 8) || !fits(config.blueBits, 8) ||
        !fits(config.alphaBits, 8) || !fits(config.depthBits, 24) || !fits(config.stencilBits, 8))
        throw tcu::NotSupportedError("ogles2.library: the default framebuffer is RGBA8, depth 24, stencil 8");
    const int samples = config.numSamples == glu::RenderConfig::DONT_CARE ? 0 : config.numSamples;
    if (samples != 0 && samples != 4)
        throw tcu::NotSupportedError("ogles2.library: 0 or 4 samples");

    const int width  = glu::getValueOrDefault(config, &glu::RenderConfig::width, 256);
    const int height = glu::getValueOrDefault(config, &glu::RenderConfig::height, 256);
    ULONG err        = 0;
    m_ctx            = aglCreateContextTags2(
        &err, OGLES2_CCT_PISTORM_WIDTH, (ULONG)width, OGLES2_CCT_PISTORM_HEIGHT, (ULONG)height, OGLES2_CCT_DEPTH,
        24UL, OGLES2_CCT_STENCIL, 8UL, OGLES2_CCT_VSYNC, (ULONG)FALSE, OGLES2_CCT_PISTORM_MSAA,
        (ULONG)(samples ? 4 : 0), TAG_DONE);
    if (!m_ctx)
        throw tcu::ResourceError("aglCreateContextTags2 failed, error " + de::toString((unsigned long)err));
    aglMakeCurrent(m_ctx);

    /* The core functions, then those of the extensions GL_EXTENSIONS lists
     * (as the other platforms' glu::initFunctions): a test of an extension's
     * entry points calls them through m_gl (debug_marker). */
    FunctionLoader loader;
    glw::initES20(&m_gl, &loader);
    glu::initExtensionFunctions(&m_gl, &loader, m_type.getAPI());

    ULONG w = 0, h = 0;
    aglSetParamsTags2(OGLES2_CCT_GET_WIDTH, (ULONG)&w, OGLES2_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
    int r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, n = 0;
    m_gl.getIntegerv(GL_RED_BITS, &r);
    m_gl.getIntegerv(GL_GREEN_BITS, &g);
    m_gl.getIntegerv(GL_BLUE_BITS, &b);
    m_gl.getIntegerv(GL_ALPHA_BITS, &a);
    m_gl.getIntegerv(GL_DEPTH_BITS, &d);
    m_gl.getIntegerv(GL_STENCIL_BITS, &s);
    m_gl.getIntegerv(GL_SAMPLES, &n);
    m_target = tcu::RenderTarget((int)w, (int)h, tcu::PixelFormat(r, g, b, a), d, s, n);
}

RenderContext::~RenderContext(void)
{
    if (m_ctx)
        aglDestroyContext(m_ctx);
}

class ContextFactory : public glu::ContextFactory
{
public:
    ContextFactory(void) : glu::ContextFactory("ogles2", "ogles2.library context in a window")
    {
    }
    glu::RenderContext *createContext(const glu::RenderConfig &config, const tcu::CommandLine &,
                                      const glu::RenderContext *sharedContext) const override
    {
        if (sharedContext)
            throw tcu::NotSupportedError("ogles2.library: shared contexts are not offered to dEQP");
        return new RenderContext(config);
    }
};

class Platform : public tcu::Platform, private glu::Platform
{
public:
    Platform(void)
    {
        if (!OGLES2Open())
            throw tcu::ResourceError("ogles2.library (1.2 or newer) did not open");
        m_contextFactoryRegistry.registerFactory(new ContextFactory());
    }
    ~Platform(void) override
    {
        OGLES2Close();
    }
    bool processEvents(void) override
    {
        return !(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C);
    }
    const glu::Platform &getGLPlatform(void) const override
    {
        return static_cast<const glu::Platform &>(*this);
    }
    void getMemoryLimits(tcu::PlatformMemoryLimits &limits) const override
    {
        limits.totalSystemMemory = 256 * 1024 * 1024;
    }
};

} // namespace agl
} // namespace tcu

tcu::Platform *createPlatform(void)
{
    return new tcu::agl::Platform();
}
