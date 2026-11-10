// Port of the RVR-Venture Homeworld GLES compatibility layer.
// Reference binary SHA256: 5887157c4045e6d6b49085e36928473ab7e33de374091a5f39f2218b3869385c.
// Reuses its shader rewrite, per-eye draw replay and single-sample fallback.
#include "hle/gles/homeworld_multiview.h"
#include "common/log.h"
#include "core/process.h"
#include "gfx/angle.h"
#include "hle/gles/gles_state.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <vector>
namespace venture::gfx::homeworld_multiview {
namespace {
void diagnostic_log(const char* format, ...) {
  char buffer[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  log::info("homeworld-multiview", "{}", buffer);
}
#define LOG(...) diagnostic_log(__VA_ARGS__)
constexpr GLenum kTextureSamplesExt = 0x8D6C;  // GL_TEXTURE_SAMPLES_EXT
constexpr GLenum kNumViews = 0x9630;           // GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR
constexpr GLenum kBaseViewIndex = 0x9632;      // GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR
constexpr const char* kViewUniform = "refract_ViewID";

template <typename T> T next(const char* name) { return reinterpret_cast<T>(angle_proc(name)); }
// Next-layer entry points, looked up on first use (extensions resolve through eglGetProcAddress).
#define NEXT(type, name) static const auto next_##name = next<type>(#name)

void log_once(const char* what, GLsizei value)
{
    static std::mutex mutex;
    static std::map<std::string, bool> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.emplace(what, true).second) LOG("%s (%d)", what, value);
}

// --- State. Framebuffers belong to one context; programs are shared, but their names are unique
// within the share group, which is all an app uses. ---
struct ViewAttachment {
    GLuint texture = 0;
    GLint level = 0, base = 0;
    GLsizei views = 0, samples = 0;
};
struct Framebuffer {
    std::map<GLenum, ViewAttachment> multiview;  // Attachment point -> emulated multiview texture.
    std::map<GLenum, GLsizei> samples;           // Pretended samples of multisampled-render-to-texture attachments.
    GLint attachedView = 0;                      // Which view's layers are attached right now.
};
struct ContextState {
    GLuint drawFramebuffer = 0, readFramebuffer = 0, program = 0;
    std::map<GLuint, Framebuffer> framebuffers;
};
std::mutex g_mutex;
std::map<EGLContext, ContextState> g_contexts;
std::map<GLuint, GLint> g_viewUniform;  // Program -> location of the view uniform.
std::map<GLuint, GLsizei> g_renderbufferSamples;

// Callers hold g_mutex.
ContextState& context() { return g_contexts[angle_function<PFNEGLGETCURRENTCONTEXTPROC, "eglGetCurrentContext">()()]; }
GLuint bound(ContextState& state, GLenum target)
{
    return target == GL_READ_FRAMEBUFFER ? state.readFramebuffer : state.drawFramebuffer;
}
Framebuffer& framebuffer(GLenum target)
{
    auto& state = context();
    return state.framebuffers[bound(state, target)];
}

void forget(GLenum target, GLenum attachment)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& fb = framebuffer(target);
    fb.multiview.erase(attachment);
    fb.samples.erase(attachment);
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        fb.multiview.erase(GL_DEPTH_ATTACHMENT);
        fb.multiview.erase(GL_STENCIL_ATTACHMENT);
    }
}

// --- Binding tracking ---
void GL_APIENTRY shim_glBindFramebuffer(GLenum target, GLuint name)
{
    NEXT(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    next_glBindFramebuffer(target, name);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = context();
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) state.drawFramebuffer = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) state.readFramebuffer = name;
}

void GL_APIENTRY shim_glDeleteFramebuffers(GLsizei count, const GLuint* names)
{
    NEXT(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    next_glDeleteFramebuffers(count, names);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = context();
    for (GLsizei i = 0; names && i < count; ++i) {
        state.framebuffers.erase(names[i]);
        if (state.drawFramebuffer == names[i]) state.drawFramebuffer = 0;
        if (state.readFramebuffer == names[i]) state.readFramebuffer = 0;
    }
}

void GL_APIENTRY shim_glUseProgram(GLuint program)
{
    NEXT(PFNGLUSEPROGRAMPROC, glUseProgram);
    next_glUseProgram(program);
    std::lock_guard<std::mutex> lock(g_mutex);
    context().program = program;
}

void remember_view_uniform(GLuint program)
{
    NEXT(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    const GLint location = next_glGetUniformLocation(program, kViewUniform);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (location >= 0) g_viewUniform[program] = location;
    else g_viewUniform.erase(program);
}

void GL_APIENTRY shim_glLinkProgram(GLuint program)
{
    NEXT(PFNGLLINKPROGRAMPROC, glLinkProgram);
    next_glLinkProgram(program);
    remember_view_uniform(program);
}

void GL_APIENTRY shim_glProgramBinary(GLuint program, GLenum format, const void* binary, GLsizei length)
{
    NEXT(PFNGLPROGRAMBINARYPROC, glProgramBinary);
    next_glProgramBinary(program, format, binary, length);
    remember_view_uniform(program);
}

// --- Multiview shaders: the view index becomes a uniform the draw loop sets. ---
void GL_APIENTRY shim_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    NEXT(PFNGLSHADERSOURCEPROC, glShaderSource);
    std::string source;
    for (GLsizei i = 0; strings && i < count; ++i) {
        if (!strings[i]) continue;
        source.append(strings[i], lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : std::strlen(strings[i]));
    }
    if (source.find("GL_OVR_multiview") == std::string::npos && source.find("gl_ViewID_OVR") == std::string::npos) {
        next_glShaderSource(shader, count, strings, lengths);
        return;
    }
    static const std::regex extension(R"(^[ \t]*#[ \t]*extension[ \t]+GL_OVR_multiview2?[ \t]*:[^\n]*)", std::regex::multiline);
    static const std::regex views(R"(layout[ \t]*\([ \t]*num_views[ \t]*=[ \t]*\d+[ \t]*\)[ \t]*in[ \t]*;)");
    std::string rewritten = std::regex_replace(std::regex_replace(source, extension, ""), views, "");
    // Declarations go after the last #version/#extension line: extension directives must come first.
    size_t insert = 0;
    static const std::regex directive(R"(^[ \t]*#[ \t]*(version|extension)[^\n]*\n?)", std::regex::multiline);
    for (auto it = std::sregex_iterator(rewritten.begin(), rewritten.end(), directive); it != std::sregex_iterator(); ++it)
        insert = static_cast<size_t>(it->position() + it->length());
    if (insert && rewritten[insert - 1] != '\n') rewritten.insert(insert++, "\n");

    // Unity places optional #extension directives inside #ifdef blocks. The
    // inserted view uniform must be outside those blocks even when the optional
    // extension is absent.
    {
        static const std::regex conditional(R"(^[ \t]*#[ \t]*(ifdef|ifndef|if|endif)\b)");
        int depth=0;size_t pos=0;
        while(pos<rewritten.size()) {
            size_t end=rewritten.find('\n',pos);if(end==std::string::npos)end=rewritten.size();else ++end;
            std::string line=rewritten.substr(pos,end-pos);std::smatch match;
            if(std::regex_search(line,match,conditional))depth+=match[1]=="endif" ? -1 : 1;
            pos=end;
            if(pos>=insert && depth==0){insert=pos;break;}
        }
    }

    rewritten.insert(insert, std::string("uniform highp uint ") + kViewUniform + ";\n#define gl_ViewID_OVR " + kViewUniform + "\n");
    log_once("rewrote a multiview shader (view index from a uniform), shader", static_cast<GLsizei>(shader));
    const GLchar* text = rewritten.c_str();
    next_glShaderSource(shader, 1, &text, nullptr);
}

// --- Attachments ---
void attach_multiview(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint base, GLsizei views, GLsizei samples)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& fb = framebuffer(target);
    if (!texture) {
        next_glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        fb.multiview.erase(attachment);
        fb.samples.erase(attachment);
        return;
    }
    // Attach the first view's layer now; draws switch layers per view.
    next_glFramebufferTextureLayer(target, attachment, texture, level, base);
    fb.multiview[attachment] = {texture, level, base, views, samples};
    fb.attachedView = 0;
    if (samples > 0) fb.samples[attachment] = samples;
    else fb.samples.erase(attachment);
}

void GL_APIENTRY shim_glFramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                       GLint baseViewIndex, GLsizei numViews)
{
    log_once("emulating glFramebufferTextureMultiviewOVR, views", numViews);
    attach_multiview(target, attachment, texture, level, baseViewIndex, numViews, 0);
}

void GL_APIENTRY shim_glFramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment, GLuint texture,
                                                                  GLint level, GLsizei samples, GLint baseViewIndex,
                                                                  GLsizei numViews)
{
    log_once("emulating glFramebufferTextureMultisampleMultiviewOVR (single-sampled), samples", samples);
    attach_multiview(target, attachment, texture, level, baseViewIndex, numViews, samples);
}

void GL_APIENTRY shim_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget,
                                                           GLuint texture, GLint level, GLsizei samples)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    log_once("glFramebufferTexture2DMultisampleEXT (single-sampled), samples", samples);
    next_glFramebufferTexture2D(target, attachment, textarget, texture, level);
    forget(target, attachment);
    if (texture && samples > 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        framebuffer(target).samples[attachment] = samples;
    }
}

void GL_APIENTRY shim_glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalformat,
                                                          GLsizei width, GLsizei height)
{
    NEXT(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    log_once("glRenderbufferStorageMultisampleEXT (single-sampled), samples", samples);
    next_glRenderbufferStorage(target, internalformat, width, height);
    GLint renderbuffer = 0;
    next_glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (samples > 0) g_renderbufferSamples[static_cast<GLuint>(renderbuffer)] = samples;
    else g_renderbufferSamples.erase(static_cast<GLuint>(renderbuffer));
}

void GL_APIENTRY shim_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    next_glFramebufferTexture2D(target, attachment, textarget, texture, level);
    forget(target, attachment);
}

void GL_APIENTRY shim_glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    next_glFramebufferTextureLayer(target, attachment, texture, level, layer);
    forget(target, attachment);
}

void GL_APIENTRY shim_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
    NEXT(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
    next_glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
    forget(target, attachment);
}

// --- Per-view replay of draws and clears on emulated multiview framebuffers ---
// Calls `draw` once per view when an emulated multiview framebuffer is bound (else once), starting
// with the view whose layers are attached so only the other view needs re-attaching.
template <typename Draw> void per_view(const char* what, Draw draw)
{
    const auto check = std::strstr(what, "Clear") ? GlCheck::clear : GlCheck::draw;
    if (!validate_gl_call(check)) return;
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    NEXT(PFNGLUNIFORM1UIPROC, glUniform1ui);
    std::vector<std::pair<GLenum, ViewAttachment>> attachments;
    GLint views = 0, first = 0, location = -1;
    Framebuffer* fb = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& state = context();
        const auto found = state.framebuffers.find(state.drawFramebuffer);
        if (state.drawFramebuffer && found != state.framebuffers.end() && !found->second.multiview.empty()) {
            fb = &found->second;
            attachments.assign(fb->multiview.begin(), fb->multiview.end());
            for (const auto& entry : attachments) views = std::max<GLint>(views, entry.second.views);
            first = std::min(fb->attachedView, views - 1);
            const auto uniform = g_viewUniform.find(state.program);
            if (uniform != g_viewUniform.end()) location = uniform->second;
        }
    }
    if (views <= 0) {
        draw();
        return;
    }
    static std::atomic<uint64_t> replayed{0};
    const uint64_t count = ++replayed;
    if ((count & (count - 1)) == 0 && count >= 64)  // 64, 128, 256, ...: proof the emulation is in the draw path.
        LOG("%llu draws/clears replayed per view (latest: %s, %d views, view uniform %s)", (unsigned long long)count, what,
            views, location >= 0 ? "set" : "absent");
    for (GLint i = 0; i < views; ++i) {
        const GLint view = (first + i) % views;
        if (i > 0)
            for (const auto& entry : attachments)
                next_glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, entry.first, entry.second.texture, entry.second.level,
                                               entry.second.base + std::min<GLint>(view, entry.second.views - 1));
        validate_gl_call(GlCheck::framebuffer_change);
        if (location >= 0) next_glUniform1ui(location, static_cast<GLuint>(view));
        draw();
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    fb->attachedView = (first + views - 1) % views;
}

void GL_APIENTRY shim_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    NEXT(PFNGLDRAWARRAYSPROC, glDrawArrays);
    per_view("glDrawArrays", [&] { next_glDrawArrays(mode, first, count); });
}
void GL_APIENTRY shim_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    NEXT(PFNGLDRAWELEMENTSPROC, glDrawElements);
    per_view("glDrawElements", [&] { next_glDrawElements(mode, count, type, indices); });
}
void GL_APIENTRY shim_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    NEXT(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced);
    per_view("glDrawArraysInstanced", [&] { next_glDrawArraysInstanced(mode, first, count, instances); });
}
void GL_APIENTRY shim_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances)
{
    NEXT(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
    per_view("glDrawElementsInstanced", [&] { next_glDrawElementsInstanced(mode, count, type, indices, instances); });
}

void GL_APIENTRY shim_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    NEXT(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex);
    if(!next_glDrawElementsBaseVertex)return;
    log_once("Homeworld base-vertex stereo replay glDrawElementsBaseVertex",basevertex);
    per_view("glDrawElementsBaseVertex",[&]{next_glDrawElementsBaseVertex(mode,count,type,indices,basevertex);});
}
void GL_APIENTRY shim_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    NEXT(PFNGLDRAWRANGEELEMENTSBASEVERTEXPROC, glDrawRangeElementsBaseVertex);
    if(!next_glDrawRangeElementsBaseVertex)return;
    log_once("Homeworld base-vertex stereo replay glDrawRangeElementsBaseVertex",basevertex);
    per_view("glDrawRangeElementsBaseVertex",[&]{next_glDrawRangeElementsBaseVertex(mode,start,end,count,type,indices,basevertex);});
}
void GL_APIENTRY shim_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint basevertex) {
    NEXT(PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXPROC, glDrawElementsInstancedBaseVertex);
    if(!next_glDrawElementsInstancedBaseVertex)return;
    log_once("Homeworld base-vertex stereo replay glDrawElementsInstancedBaseVertex",basevertex);
    per_view("glDrawElementsInstancedBaseVertex",[&]{next_glDrawElementsInstancedBaseVertex(mode,count,type,indices,instances,basevertex);});
}

void GL_APIENTRY shim_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices)
{
    NEXT(PFNGLDRAWRANGEELEMENTSPROC, glDrawRangeElements);
    per_view("glDrawRangeElements", [&] { next_glDrawRangeElements(mode, start, end, count, type, indices); });
}
void GL_APIENTRY shim_glDrawArraysIndirect(GLenum mode, const void* indirect)
{
    NEXT(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect);
    per_view("glDrawArraysIndirect", [&] { next_glDrawArraysIndirect(mode, indirect); });
}
void GL_APIENTRY shim_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect)
{
    NEXT(PFNGLDRAWELEMENTSINDIRECTPROC, glDrawElementsIndirect);
    per_view("glDrawElementsIndirect", [&] { next_glDrawElementsIndirect(mode, type, indirect); });
}
void GL_APIENTRY shim_glClear(GLbitfield mask)
{
    NEXT(PFNGLCLEARPROC, glClear);
    per_view("glClear", [&] { next_glClear(mask); });
}
void GL_APIENTRY shim_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value)
{
    NEXT(PFNGLCLEARBUFFERFVPROC, glClearBufferfv);
    per_view("glClearBufferfv", [&] { next_glClearBufferfv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value)
{
    NEXT(PFNGLCLEARBUFFERIVPROC, glClearBufferiv);
    per_view("glClearBufferiv", [&] { next_glClearBufferiv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value)
{
    NEXT(PFNGLCLEARBUFFERUIVPROC, glClearBufferuiv);
    per_view("glClearBufferuiv", [&] { next_glClearBufferuiv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    NEXT(PFNGLCLEARBUFFERFIPROC, glClearBufferfi);
    per_view("glClearBufferfi", [&] { next_glClearBufferfi(buffer, drawbuffer, depth, stencil); });
}

// --- Queries ---
void GL_APIENTRY shim_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params)
{
    NEXT(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC, glGetFramebufferAttachmentParameteriv);
    if (params && (pname == kTextureSamplesExt || pname == kNumViews || pname == kBaseViewIndex)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& fb = framebuffer(target);
        if (pname == kTextureSamplesExt) {
            const auto found = fb.samples.find(attachment);
            *params = found == fb.samples.end() ? 0 : found->second;
        } else {
            const auto found = fb.multiview.find(attachment);
            if (found == fb.multiview.end()) *params = pname == kNumViews ? 1 : 0;
            else *params = pname == kNumViews ? found->second.views : found->second.base;
        }
        return;
    }
    next_glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

void GL_APIENTRY shim_glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    NEXT(PFNGLGETRENDERBUFFERPARAMETERIVPROC, glGetRenderbufferParameteriv);
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    next_glGetRenderbufferParameteriv(target, pname, params);
    if (pname == GL_RENDERBUFFER_SAMPLES && params) {
        GLint renderbuffer = 0;
        next_glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_renderbufferSamples.find(static_cast<GLuint>(renderbuffer));
        if (found != g_renderbufferSamples.end()) *params = found->second;
    }
}


#undef NEXT
#undef LOG
} // namespace
bool enabled() {
  static const bool value = process_config().isa() == GuestIsa::a64 &&
      process_config().package == "com.farbridge.vastreaches";
  return value;
}
hle::Handler handler(std::string_view name) {
  if (!enabled()) return nullptr;
  if (name == "glFramebufferTexture2DMultisampleEXT") return wrap<&shim_glFramebufferTexture2DMultisampleEXT>();
  if (name == "glRenderbufferStorageMultisampleEXT") return wrap<&shim_glRenderbufferStorageMultisampleEXT>();
  if (name == "glFramebufferTextureMultisampleMultiviewOVR") return wrap<&shim_glFramebufferTextureMultisampleMultiviewOVR>();
  if (name == "glFramebufferTextureMultiviewOVR") return wrap<&shim_glFramebufferTextureMultiviewOVR>();
  if (name == "glFramebufferTexture2D") return wrap<&shim_glFramebufferTexture2D>();
  if (name == "glFramebufferTextureLayer") return wrap<&shim_glFramebufferTextureLayer>();
  if (name == "glFramebufferRenderbuffer") return wrap<&shim_glFramebufferRenderbuffer>();
  if (name == "glBindFramebuffer") return wrap<&shim_glBindFramebuffer>();
  if (name == "glDeleteFramebuffers") return wrap<&shim_glDeleteFramebuffers>();
  if (name == "glUseProgram") return wrap<&shim_glUseProgram>();
  if (name == "glLinkProgram") return wrap<&shim_glLinkProgram>();
  if (name == "glProgramBinary") return wrap<&shim_glProgramBinary>();
  if (name == "glShaderSource") return wrap<&shim_glShaderSource>();
  if (name == "glDrawArrays") return wrap<&shim_glDrawArrays>();
  if (name == "glDrawElements") return wrap<&shim_glDrawElements>();
  if (name == "glDrawArraysInstanced") return wrap<&shim_glDrawArraysInstanced>();
  if (name == "glDrawElementsInstanced") return wrap<&shim_glDrawElementsInstanced>();
  if (name == "glDrawRangeElements") return wrap<&shim_glDrawRangeElements>();
  if (name == "glDrawArraysIndirect") return wrap<&shim_glDrawArraysIndirect>();
  if (name == "glDrawElementsIndirect") return wrap<&shim_glDrawElementsIndirect>();
  if (name == "glDrawElementsBaseVertex" || name == "glDrawElementsBaseVertexEXT" || name == "glDrawElementsBaseVertexOES") return wrap<&shim_glDrawElementsBaseVertex>();
  if (name == "glDrawRangeElementsBaseVertex" || name == "glDrawRangeElementsBaseVertexEXT" || name == "glDrawRangeElementsBaseVertexOES") return wrap<&shim_glDrawRangeElementsBaseVertex>();
  if (name == "glDrawElementsInstancedBaseVertex" || name == "glDrawElementsInstancedBaseVertexEXT" || name == "glDrawElementsInstancedBaseVertexOES") return wrap<&shim_glDrawElementsInstancedBaseVertex>();
  if (name == "glClear") return wrap<&shim_glClear>();
  if (name == "glClearBufferfv") return wrap<&shim_glClearBufferfv>();
  if (name == "glClearBufferiv") return wrap<&shim_glClearBufferiv>();
  if (name == "glClearBufferuiv") return wrap<&shim_glClearBufferuiv>();
  if (name == "glClearBufferfi") return wrap<&shim_glClearBufferfi>();
  if (name == "glGetFramebufferAttachmentParameteriv") return wrap<&shim_glGetFramebufferAttachmentParameteriv>();
  if (name == "glGetRenderbufferParameteriv") return wrap<&shim_glGetRenderbufferParameteriv>();
  return nullptr;
}
} // namespace venture::gfx::homeworld_multiview
