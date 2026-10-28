# Guest Vulkan

Link `venture_vulkan` and register `venture::vulkan::register_library()` before loading
guest libraries. Production and tests use the integrated core, graphics and
native-window implementations. Allocation/debug callbacks execute ARM64 thunks
through Dynarmic; GPU tests are in the `gpu` doctest suite.

The registry generates 778 command handlers (core 1.0â€“1.4, aliases, and
non-provisional portable/Android extensions) and 1,168 structure layouts. Guest
exports and procedure queries always return ARM64 HLE thunks. No host procedure
address is exposed to translated ARM64 code. Handles retain host identity.

Instances and devices belong to the guest. Physical-device enumeration selects
the broker's GPU by device UUID across instances. Reusing the shared logical
device would lose the guest's enabled-feature/extension and allocator contracts.
Submitters using the shared queue take `GpuDevice::queue_lock`.

For OpenXR enable2, replace the guest `pfnGetInstanceProcAddr` with the **host**
`xr_host_proc` resolver. It intercepts final instance/device creation after the
runtime adds its requirements, including platform extensions hidden from guests.
Do not pass the guest resolver directly to the native XR runtime. A broker using
its own creation implementation can instead call `adopt_instance`,
`adopt_physical_device`, and `adopt_device` with the actual creation descriptions.
The instance must be registered before its physical device and logical device.
The XR session must use the guest's device for guest Vulkan swapchain images;
images from ANGLE's separate device are not interchangeable.

Allocation/debug callback records copy the guest addresses and userdata, remain
stable for the process lifetime, and enter guest code through
`GuestThread::adopt_current().call()`. Interning avoids driver-worker callbacks
retaining pointers to temporary guest structures. This deliberately retains a
small record per distinct callback tuple until process exit.

ASTC LDR, ETC2 and EAC formats unsupported natively use decoded host images and
a GPU buffer preserving the original compressed blocks. Uploads record copies
and compute decode in command-stream order, including strided partial regions,
mips and array layers. Same-format image copies copy compressed blocks and decode
the destination; guest readback returns compressed bytes. Linear ASTC uses exact
FP16 decoding; sRGB uses sRGB RGBA8. EAC uses R32/RG32 float backing to preserve
signed/unsigned normalized values. Temporary buffers/descriptors live until the
command buffer is reset/freed. Compute bindings and push constants are restored
after inserted work. See `shaders/NOTICE.md` for source pins and licensing.

Current limitations are explicit failures, not successful placeholder objects:

- Android surfaces require host `VK_EXT_headless_surface`. Drivers without it
  do not advertise guest `VK_KHR_android_surface`. A native mirror-window backend
  is still needed on those drivers. Native windows are validated and retained.
- `VK_ANDROID_external_memory_android_hardware_buffer` (with
  `VK_EXT_queue_family_foreign`, translated to `VK_QUEUE_FAMILY_EXTERNAL` when
  the host lacks it) is advertised when the host exports memory as platform
  handles (Windows: OPAQUE_WIN32). GPU-only AHardwareBuffers of the spec's
  format table are dedicated image payloads; images bound to them use one
  canonical create info per shape, as opaque imports require. CPU-access and
  BLOB/data-buffer AHBs, and imports whose mip chain differs from the exporting
  image's, are refused. RGB external formats sample directly; their YCbCr
  conversions carry only the component mapping. SYNC_FD semaphores and fences
  are guest sync files. Android's Vulkan 1.1 feature flag is reported only with
  all of these and samplerYcbcrConversion. Native buffers and Google display
  timing are hidden/rejected. An external format of zero is removed from chains.
- QCOM extensions, unbridged callback/external-handle APIs, descriptor-buffer/
  descriptor-heap and shader-object extensions are hidden. Density-map features
  remain native-only; nothing spoofs an Adreno GPU or emulates foveation.
- Emulated compressed images currently support optimal 2D/cube/array and
  sliced 3D (2D blocks per depth slice, optionally 2D-array compatible) sampled
  images and transfers, one sample, and compute-capable upload queues. Sparse,
  protected, aliased images, host image copies, 3D-block/HDR ASTC, mixed
  compressed/uncompressed image copies and image chains are not implemented;
  views accept usage, min-LOD and ASTC decode-mode structures only. Queries reject unsupported image shapes/flags/usages. The
  decoder's temporary storage is bounded by `maxStorageBufferRange`.
- Push descriptor extension payloads (e.g. acceleration-structure writes) are
  rejected; ordinary image/buffer/texel-buffer pushes and update templates are
  preserved. Invalid compressed blocks have not received a full CTS corpus.
- No end-to-end game launch, headset rendering, Linux run, Vulkan CTS or
  validation-layer certification is claimed. This branch has no real core/GPU
  implementation. Static APK procedure-name scans are lookup coverage only.

Build/test using the repository's Windows release presets and `venture_tests.exe`.
The tests use the real core for HLE calls and real Vulkan driver work. GPU ASTC tests
compare against pinned astcenc for every LDR footprint in linear and sRGB modes;
ETC2/EAC vectors cover alpha, signedness and sRGB. Texture tests include
GPU-produced data, partial strided uploads, mip/layer copies, byte-preserving
readback, and replay with changed source bytes.
