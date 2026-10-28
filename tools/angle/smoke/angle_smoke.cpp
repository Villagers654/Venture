// Native C-ABI proof for the Venture ANGLE patch. No ANGLE/Vulkan import libs.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle_venture.h>
#include <GLES3/gl32.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class Library {
 public:
  explicit Library(const std::filesystem::path& path, bool system = false) {
#ifdef _WIN32
    handle_ = LoadLibraryExW(path.c_str(), nullptr, system ? LOAD_LIBRARY_SEARCH_SYSTEM32
                  : LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(handle_ != nullptr, "LoadLibrary failed: " + path.string() +
                                " (" + std::to_string(GetLastError()) + ")");
#else
    (void)system;
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(handle_ != nullptr, "dlopen failed: " + path.string());
#endif
  }
  ~Library() {
#ifdef _WIN32
    if (handle_) FreeLibrary(handle_);
#else
    if (handle_) dlclose(handle_);
#endif
  }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  template <typename T> T symbol(const char* name) const {
#ifdef _WIN32
    auto pointer = GetProcAddress(handle_, name);
#else
    auto pointer = dlsym(handle_, name);
#endif
    require(pointer != nullptr, std::string("Missing symbol: ") + name);
    return reinterpret_cast<T>(pointer);
  }
 private:
#ifdef _WIN32
  HMODULE handle_{};
#else
  void* handle_{};
#endif
};

struct CallbackState {
  PFN_vkGetInstanceProcAddr loader{};
  std::uint32_t vendor{};
  std::string_view negative;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  std::uint32_t family{}, index{};
  int instances{}, selections{}, devices{}, ready{};
};

VkResult VKAPI_CALL create_instance(void* data, PFN_vkGetInstanceProcAddr loader,
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
  auto& state = *static_cast<CallbackState*>(data);
  ++state.instances;
  if (state.negative == "instance" || loader != state.loader) return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(loader(nullptr, "vkCreateInstance"));
  auto result = create(info, allocator, instance);
  if (result == VK_SUCCESS) state.instance = *instance;
  std::printf("createInstance: VkResult=%d, requested extensions=%u, api=%u.%u\n",
              result, info->enabledExtensionCount,
              VK_API_VERSION_MAJOR(info->pApplicationInfo->apiVersion),
              VK_API_VERSION_MINOR(info->pApplicationInfo->apiVersion));
  return result;
}

VkResult VKAPI_CALL choose_physical(void* data, PFN_vkGetInstanceProcAddr loader,
    VkInstance instance, std::uint32_t count, const VkPhysicalDevice* devices,
    VkPhysicalDevice* selected) {
  auto& state = *static_cast<CallbackState*>(data);
  ++state.selections;
  if (state.negative == "physical") {
    *selected = VK_NULL_HANDLE;  // Success with an invalid handle must still be rejected.
    return VK_SUCCESS;
  }
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      loader(instance, "vkGetPhysicalDeviceProperties"));
  for (std::uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties p{};
    properties(devices[i], &p);
    std::printf("Vulkan device: %s (vendor 0x%04x/device 0x%04x)\n", p.deviceName, p.vendorID, p.deviceID);
    if ((state.vendor != 0 && p.vendorID == state.vendor) ||
        (state.vendor == 0 && (state.physical == nullptr || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)))
      state.physical = devices[i];
  }
  *selected = state.physical;
  return state.physical ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VKAPI_CALL create_device(void* data, PFN_vkGetInstanceProcAddr loader,
    VkInstance instance, VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
  auto& state = *static_cast<CallbackState*>(data);
  ++state.devices;
  if (state.negative == "device") return VK_ERROR_INITIALIZATION_FAILED;
  // Callback mode promises a single unprotected queue, with ANGLE's requirements intact.
  if (physical != state.physical || info->queueCreateInfoCount != 1 ||
      info->pQueueCreateInfos[0].queueCount != 1 || info->pQueueCreateInfos[0].flags != 0)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateDevice>(loader(instance, "vkCreateDevice"));
  auto result = create(physical, info, allocator, device);
  if (result == VK_SUCCESS) state.device = *device;
  std::printf("createDevice: VkResult=%d, requested extensions=%u\n", result, info->enabledExtensionCount);
  return result;
}

void VKAPI_CALL device_ready(void* data, VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, std::uint32_t family, std::uint32_t index, VkQueue queue) {
  auto& state = *static_cast<CallbackState*>(data);
  ++state.ready;
  if (instance != state.instance || physical != state.physical || device != state.device) {
    state.ready = -100;
    return;
  }
  state.family = family;
  state.index = index;
  state.queue = queue;
}

struct Egl {
  explicit Egl(const Library& library) : library(library) {}
  const Library& library;
#define EGL_PROC(name) decltype(&::name) name = library.symbol<decltype(&::name)>(#name)
  EGL_PROC(eglGetProcAddress);
  EGL_PROC(eglGetPlatformDisplay);
  EGL_PROC(eglInitialize);
  EGL_PROC(eglTerminate);
  EGL_PROC(eglGetError);
  EGL_PROC(eglQueryString);
  EGL_PROC(eglChooseConfig);
  EGL_PROC(eglBindAPI);
  EGL_PROC(eglCreateContext);
  EGL_PROC(eglDestroyContext);
  EGL_PROC(eglMakeCurrent);
#undef EGL_PROC
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  ~Egl() {
    if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
      eglTerminate(display);
    }
  }
  template <typename T> T proc(const char* name) const {
    auto pointer = eglGetProcAddress(name);
    require(pointer != nullptr, std::string("Missing EGL procedure: ") + name);
    return reinterpret_cast<T>(pointer);
  }
  void check(bool ok, const char* name) const {
    if (!ok) {
      char error[16];
      std::snprintf(error, sizeof(error), "0x%04x", eglGetError());
      throw std::runtime_error(std::string(name) + " failed: EGL error " + error);
    }
  }
};

void render_and_probe(const Library& gles, int requested_minor) {
#define GL_PROC(name) auto name = gles.symbol<decltype(&::name)>(#name)
  GL_PROC(glGetString); GL_PROC(glGetStringi); GL_PROC(glGetIntegerv); GL_PROC(glGetError);
  GL_PROC(glGenTextures); GL_PROC(glBindTexture); GL_PROC(glTexImage2D); GL_PROC(glTexParameteri);
  GL_PROC(glGenFramebuffers); GL_PROC(glBindFramebuffer); GL_PROC(glFramebufferTexture2D);
  GL_PROC(glCheckFramebufferStatus); GL_PROC(glViewport); GL_PROC(glClearColor); GL_PROC(glClear);
  GL_PROC(glCreateShader); GL_PROC(glShaderSource); GL_PROC(glCompileShader);
  GL_PROC(glGetShaderiv); GL_PROC(glGetShaderInfoLog); GL_PROC(glCreateProgram);
  GL_PROC(glAttachShader); GL_PROC(glLinkProgram); GL_PROC(glGetProgramiv); GL_PROC(glGetProgramInfoLog);
  GL_PROC(glUseProgram); GL_PROC(glGetUniformLocation); GL_PROC(glUniform1i);
  GL_PROC(glGenVertexArrays); GL_PROC(glBindVertexArray); GL_PROC(glDrawArrays); GL_PROC(glReadPixels);
  GL_PROC(glDeleteTextures); GL_PROC(glDeleteFramebuffers); GL_PROC(glDeleteVertexArrays);
  GL_PROC(glDeleteShader); GL_PROC(glDeleteProgram); GL_PROC(glFinish);
#undef GL_PROC
  std::cout << "GL_VERSION: " << glGetString(GL_VERSION) << '\n'
            << "GL_RENDERER: " << glGetString(GL_RENDERER) << '\n';
  GLint major{}, minor{}, count{};
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  require(major > 3 || (major == 3 && minor >= requested_minor), "Requested GLES version was not created");
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  std::set<std::string> extensions;
  for (GLint i = 0; i < count; ++i)
    extensions.emplace(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))));
  constexpr const char* queried[] = {
    "GL_OVR_multiview2", "GL_EXT_multisampled_render_to_texture", "GL_EXT_multisampled_render_to_texture2",
    "GL_OES_EGL_image_external", "GL_OES_EGL_image_external_essl3", "GL_EXT_texture_border_clamp",
    "GL_EXT_shader_framebuffer_fetch", "GL_KHR_texture_compression_astc_ldr",
    "GL_OES_compressed_ETC2_RGB8_texture", "GL_OES_compressed_ETC2_sRGB8_texture",
    "GL_OES_compressed_ETC2_RGBA8_texture", "GL_OES_compressed_ETC2_sRGB8_alpha8_texture",
    "GL_OES_compressed_ETC2_punchthroughA_RGBA8_texture",
    "GL_OES_compressed_ETC2_punchthroughA_sRGB8_alpha_texture",
  };
  for (auto name : queried) std::cout << name << ": " << (extensions.contains(name) ? "yes" : "no") << '\n';
  for (const auto& name : extensions)
    if (name.starts_with("GL_OES_compressed_ETC2")) std::cout << "ETC2 extension: " << name << '\n';
  std::cout << "ETC2 formats are also core in GLES 3.x; extension absence is not format absence.\n";

  GLuint target{}, sampled{}, framebuffer{}, vao{};
  glGenTextures(1, &target);
  glBindTexture(GL_TEXTURE_2D, target);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
  require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete texture framebuffer");
  glViewport(0, 0, 32, 32);
  glClearColor(0, 1, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  auto pixel = [&](int x, int y, std::array<GLubyte, 4> expected) {
    std::array<GLubyte, 4> actual{};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    if (actual != expected) {
      std::printf("Unexpected pixel at %d,%d: %u,%u,%u,%u\n", x, y,
                  actual[0], actual[1], actual[2], actual[3]);
      throw std::runtime_error("Pixel readback mismatch");
    }
  };
  pixel(16, 16, {0, 255, 0, 255});
  glGenTextures(1, &sampled);
  glBindTexture(GL_TEXTURE_2D, sampled);
  // Lower-left red, lower-right blue, upper-left yellow, upper-right white.
  const GLubyte texels[] = {255,0,0,255, 0,0,255,255, 255,255,0,255, 255,255,255,255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  const std::string version = requested_minor == 2 ? "#version 320 es\n" : "#version 310 es\n";
  const std::string vertex = version + R"(out vec2 uv;
void main() {
  vec2 p[3] = vec2[3](vec2(-.75,-.75), vec2(.75,-.75), vec2(-.75,.75));
  gl_Position = vec4(p[gl_VertexID],0,1);
  uv = (p[gl_VertexID]+.75)/1.5;
})";
  const std::string fragment = version + R"(precision highp float;
in vec2 uv;
uniform sampler2D tex;
layout(location=0) out vec4 color;
void main() { color = texture(tex,uv); }
)";
  auto compile = [&](GLenum type, const char* source) {
    auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok{};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    std::array<char, 4096> log{};
    if (!ok) glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
    require(ok != 0, std::string("Shader compilation failed: ") + log.data());
    return shader;
  };
  auto vs = compile(GL_VERTEX_SHADER, vertex.c_str());
  auto fs = compile(GL_FRAGMENT_SHADER, fragment.c_str());
  auto program = glCreateProgram();
  glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
  GLint linked{};
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  std::array<char, 4096> log{};
  if (!linked) glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
  require(linked != 0, std::string("Program link failed: ") + log.data());
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "tex"), 0);
  glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  pixel(1, 1, {0, 255, 0, 255});
  pixel(8, 8, {255, 0, 0, 255});
  pixel(20, 8, {0, 0, 255, 255});
  pixel(8, 20, {255, 255, 0, 255});
  pixel(28, 28, {0, 255, 0, 255});
  glFinish();
  require(glGetError() == GL_NO_ERROR, "GLES reported an error");
  glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
  glDeleteShader(vs); glDeleteShader(fs);
  glDeleteTextures(1, &sampled); glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &target);
  std::cout << "PASS: clear and textured triangle rendered into RGBA8 texture; six exact pixel checks\n";
}

int run(int argc, char** argv) {
  require(argc >= 2, "Usage: venture_angle_smoke <ANGLE bin directory> [--vendor 0x10de|0x8086] [--gles31] [--stock] [--negative backend|version|size|null|instance|physical|device]");
  auto directory = std::filesystem::absolute(argv[1]);
  CallbackState state;
  bool stock = false;
  int requested_minor = 2;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--stock") stock = true;
    else if (arg == "--gles31") requested_minor = 1;
    else if (arg == "--vendor" && i + 1 < argc) state.vendor = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
    else if (arg == "--negative" && i + 1 < argc) state.negative = argv[++i];
    else throw std::runtime_error("Unknown or incomplete option: " + std::string(arg));
  }
  const std::set<std::string_view> modes{"", "backend", "version", "size", "null", "instance", "physical", "device"};
  require(modes.contains(state.negative) && !(stock && !state.negative.empty()), "Invalid test mode");
#ifdef _WIN32
  Library vulkan("vulkan-1.dll", true);
  Library gles(directory / "libGLESv2.dll");
  Library egl_library(directory / "libEGL.dll");
#else
  Library vulkan("libvulkan.so.1", true);
  Library gles(directory / "libGLESv2.so");
  Library egl_library(directory / "libEGL.so");
#endif
  state.loader = vulkan.symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  EGLVulkanCreationCallbacksVENTURE callbacks{1, sizeof(EGLVulkanCreationCallbacksVENTURE),
      &state, create_instance, choose_physical, create_device, device_ready};
  Egl egl(egl_library);
  const char* client_extensions = egl.eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  constexpr auto legacy_extension = "EGL_QUESTPORT_vulkan_creation_callbacks"; // rebrand: legacy verified ANGLE ABI
  require(client_extensions && (std::string_view(client_extensions).find("EGL_VENTURE_vulkan_creation_callbacks") != std::string_view::npos ||
                               std::string_view(client_extensions).find(legacy_extension) != std::string_view::npos),
          "Venture callback extension missing");
  if (state.negative == "version") callbacks.version = 999;
  if (state.negative == "size") callbacks.size = 8;
  if (state.negative == "null") callbacks.createDevice = nullptr;
  std::vector<EGLAttrib> attributes{
      EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
      EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE, EGL_FALSE};
  if (!stock) {
    attributes.push_back(EGL_PLATFORM_ANGLE_VULKAN_CREATION_CALLBACKS_VENTURE);
    attributes.push_back(reinterpret_cast<EGLAttrib>(&callbacks));
  }
  if (state.vendor != 0) {
    attributes.push_back(EGL_PLATFORM_ANGLE_DEVICE_ID_HIGH_ANGLE);
    attributes.push_back(state.vendor);
  }
  attributes.push_back(EGL_NONE);
  if (state.negative == "backend") attributes[1] = EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE;
  egl.display = egl.eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes.data());
  if (state.negative == "backend") {
    require(egl.display == EGL_NO_DISPLAY && egl.eglGetError() == EGL_BAD_ATTRIBUTE && state.instances == 0,
            "Callback attribute accepted without explicit Vulkan backend");
    std::cout << "PASS: callback attribute rejected without explicit Vulkan backend\n";
    return 0;
  }
  egl.check(egl.display != EGL_NO_DISPLAY, "eglGetPlatformDisplay");
  require(egl.display == egl.eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes.data()),
          "Identical callback pointer did not reuse display");
  if (!stock) {
    auto another = callbacks;
    attributes[5] = reinterpret_cast<EGLAttrib>(&another);
    auto other_display = egl.eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes.data());
    require(other_display != EGL_NO_DISPLAY && other_display != egl.display, "Different callback tables aliased displays");
    egl.eglTerminate(other_display);
  }
  EGLint major{}, minor{};
  bool initialized = egl.eglInitialize(egl.display, &major, &minor) == EGL_TRUE;
  if (!state.negative.empty()) {
    require(!initialized, "Negative callback test unexpectedly initialized EGL");
    EGLint error = egl.eglGetError();
    bool invalid = state.negative == "version" || state.negative == "size" || state.negative == "null";
    require(error == (invalid ? EGL_BAD_ATTRIBUTE : EGL_NOT_INITIALIZED), "Wrong EGL failure code");
    require(state.instances == (invalid ? 0 : 1) && state.selections ==
        ((!invalid && state.negative != "instance") ? 1 : 0) &&
        state.devices == (state.negative == "device" ? 1 : 0) && state.ready == 0,
        "Failure did not stop the callback chain");
    std::cout << "PASS: negative " << state.negative << " propagated with expected EGL error\n";
    return 0;
  }
  egl.check(initialized, "eglInitialize");
  auto query_display = egl.proc<PFNEGLQUERYDISPLAYATTRIBEXTPROC>("eglQueryDisplayAttribEXT");
  auto query_device = egl.proc<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
  EGLAttrib egl_device{};
  egl.check(query_display(egl.display, EGL_DEVICE_EXT, &egl_device), "eglQueryDisplayAttribEXT");
  auto query = [&](EGLint token) {
    EGLAttrib result{};
    egl.check(query_device(reinterpret_cast<EGLDeviceEXT>(egl_device), token, &result), "eglQueryDeviceAttribEXT");
    return result;
  };
  if (!stock) {
    require(state.instances == 1 && state.selections == 1 && state.devices == 1 && state.ready == 1, "Callbacks not each invoked once");
    require(query(EGL_VULKAN_INSTANCE_ANGLE) == reinterpret_cast<EGLAttrib>(state.instance) &&
            query(EGL_VULKAN_PHYSICAL_DEVICE_ANGLE) == reinterpret_cast<EGLAttrib>(state.physical) &&
            query(EGL_VULKAN_DEVICE_ANGLE) == reinterpret_cast<EGLAttrib>(state.device) &&
            query(EGL_VULKAN_QUEUE_ANGLE) == reinterpret_cast<EGLAttrib>(state.queue) &&
            query(EGL_VULKAN_QUEUE_FAMILIY_INDEX_ANGLE) == state.family &&
            query(EGL_VULKAN_QUEUE_INDEX_VENTURE) == state.index,
            "ANGLE device queries differ from callback handles/queue");
    auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(state.loader(state.instance, "vkGetDeviceQueue"));
    VkQueue actual{};
    get_queue(state.device, state.family, state.index, &actual);
    require(actual == state.queue, "Reported family/index did not resolve to ANGLE's queue");
    std::cout << "PASS: callback handles match EGL queries; queue family=" << state.family << " index=" << state.index << '\n';
  }
  egl.check(egl.eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
  const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config{};
  EGLint count{};
  egl.check(egl.eglChooseConfig(egl.display, config_attributes, &config, 1, &count) && count == 1, "eglChooseConfig");
  const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, requested_minor, EGL_NONE};
  egl.context = egl.eglCreateContext(egl.display, config, EGL_NO_CONTEXT, context_attributes);
  egl.check(egl.context != EGL_NO_CONTEXT, requested_minor == 2 ? "eglCreateContext GLES 3.2" : "eglCreateContext GLES 3.1");
  egl.check(egl.eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.context), "surfaceless eglMakeCurrent");
  render_and_probe(gles, requested_minor);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try { return run(argc, argv); }
  catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
