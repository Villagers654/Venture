# Graphics integration

Desktop Vulkan enables ANGLE's `allowMultisampledRenderToTextureEmulation`
feature through `EGL_ANGLE_feature_control`. ANGLE owns the transient multisample
attachments and implicit resolves for `GL_EXT_multisampled_render_to_texture`;
its Vulkan depth/stencil resolve capability checks still determine exposure.
This does not depend on a GPU vendor or report an extension without an implementation.

Link `venture_gfx` and call `register_graphics_libraries()` before resolving guest
imports. It registers `libEGL.so` and `libGLESv2.so` (alias `libGLESv3.so`). The
902 GLES signatures and extension command requirements are generated at build
time from pinned, SHA-256 checked Khronos registries. There is no host procedure
address exposed to guest code. This is the AArch64 ABI; ARM32 is not implemented.

Set `VENTURE_ANGLE_DIR`, or call `configure_angle(directory)`, before the first graphics
operation. The directory must contain the matching ANGLE EGL/GLES libraries.
Vulkan is selected explicitly. A stock ANGLE build is allowed in flat mode only;
its actual instance, device, queue, enabled extensions and API version are queried.
The medium queue must be verified to be queue index zero. Other configurations
require the creation callback interface.

`angle_callbacks.h` matches the ANGLE branch's version-1
`EGLVulkanCreationCallbacksVENTURE` host ABI. The loader checks its private
client extension and passes the table via `eglGetPlatformDisplay` with
pointer-sized `EGLAttrib` entries and the private `0x51500001` attribute. Stock
ANGLE receives only the ordinary Vulkan platform attributes.
Callbacks retain ANGLE's feature chains and extension requirements, create through
OpenXR enable2 when available, and report the queue family **and index**.
`request_gpu_extensions()` adds guest-Vulkan requirements before initialization;
late requests fail. The recording Vulkan resolver observes extensions added by
the XR runtime at the actual Vulkan creation call. ANGLE owns callback-created
objects; guest `eglTerminate` only destroys guest EGL resources. The direct device
is process-owned. Initialize XR/configuration before either graphics API starts.

`xr_images.h` documents the XR agent's acquire/wait/render/release sequence.
External queue operations lock `GpuDevice::queue_lock` and then ANGLE's queue
lock. GL calls must happen outside ANGLE's nonrecursive lock. Bridge release
submits copies and fences; reuse waits for its own fence, with signal delivery,
without an unconditional queue idle. Arrays use one extra GL copy per layer.
The first bridge supports single-sample color, one mip, RGBA8, sRGB RGBA8,
RGBA16F and RGB10_A2. Depth, MSAA swapchain targets, BGRA conversion and zero-copy
array import are rejected. A headset session must be tested after the ANGLE and
XR branches are integrated.

EGL windows retain `NativeWindow` references and use real pbuffers of the initial
window size. Desktop mirroring and resizing an existing window surface are not
implemented. Context priority requests are advisory and report medium. Android
native buffers, native-fence fds, presentation timestamps, recordable surfaces,
front-buffer refresh and protected content are hidden/rejected. Fence/wait sync,
GL texture EGLImages and per-host-thread contexts are implemented.

GLES extension strings and indexed/count queries use the same filtered list.
Untranslated OS-handle APIs, QCOM hints and ANGLE-private commands are hidden.
GL_RENDERER reports the selected headset's Adreno, as Build and VrApi do, because
UE4 picks its Android device profile from it. GL_VENDOR and program-binary formats
describe the actual ANGLE implementation, and GL_VERSION is ANGLE's with a token
hashed from ANGLE's real renderer string appended: UE4 keys its program-binary
cache on GL_VERSION and GL_RENDERER, so a host GPU, driver or SwiftShader switch
starts a new cache instead of loading binaries ANGLE rejects. Debug callbacks translate through `GuestThread::call`, adopting
host-created threads when necessary. Small callback records remain alive until
process exit because a driver worker may retain userdata after context deletion.

Tests use the real core, linker, guest callbacks and native windows. Only the
explicit enable2 dispatch fixture is simulated; it never opens OpenXR.
The runtime currently links a flat-mode host_xr implementation while XR is
unmerged. CPU decoder tests run with `--test-suite-exclude=gpu`.

Build with the normal `windows-release` presets. Run `venture_tests.exe` without
`VENTURE_ANGLE_DIR` for the direct Vulkan path, and with it for HLE/ANGLE hardware tests.
Without that variable, ANGLE-specific tests print a message and return early.
With the patched ANGLE, `VENTURE_GFX_FAKE_XR=1` routes creation through a tests-only
enable2 implementation that adds extension requirements and forwards to the real
Vulkan loader. It verifies all four calls and the final extension records; it is
not a headset/runtime test.
`VENTURE_GFX_PROCS_OUT=<path>` writes resolved procedure names during the smoke test;
`tools/codegen/gl_game_surface.py` compares them to the measured r3 inventory and
can reopen all owned APKs to verify the ELF hashes. That audit checks static
symbol coverage, not game startup or ARM32 execution. POSIX dynamic loading is
implemented; a Linux toolchain/runtime was unavailable on this machine.
