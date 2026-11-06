# Compressed textures

Prefer ANGLE's native formats and its own Vulkan format fallbacks. The measured
RTX 5090 lacks ASTC and ETC2 in Vulkan (`r3b-angle-vulkan/report.md`). The stock
ANGLE used in local tests decodes ETC2/EAC, including the R11/RG11 channel
semantics, but omits ASTC. ETC2/EAC therefore stay in ANGLE; they are not reduced
to a generic four-channel decoder. A future ANGLE build must retain its ETC2/EAC
conversion support to provide an ES3 context.

When ANGLE lacks `GL_KHR_texture_compression_astc_ldr`, the HLE decodes all 14
2D footprints and both color spaces with pinned astcenc 5.3.0, decompression-only,
baseline SSE2. Decoder contexts are cached by footprint/profile/worker count.
Large images use up to eight workers. Linear textures use RGBA8; sRGB textures
use SRGB8_ALPHA8. The original per-level format remains in a share-group table
for texture queries, subimage block validation and compressed-renderability
rules. Full images, immutable mip storage, cube faces, 2D/cube arrays, subimages,
and pixel-unpack-buffer sources are handled. Uncompressed edits and mipmap
generation on compressed images fail as required by GLES. LDR alone does not
enable `TEXTURE_3D`; HDR and sliced-3D support are not added by this fallback.

Sub-image uploads on the BC7 block grid (and every RGBA8 sub-image upload)
return at once: the HLE copies the ASTC data and decodes it on background
threads. The GL upload waits until a draw or compute dispatch runs with the
texture bound in that context (any unit), the guest uses the texture in another
way (a new image or storage, copies, views, EGL images), or the decode is done
and the render thread has time (about 1 ms in each 10 ms). Uploads of one
texture keep their order, a deleted texture's uploads are dropped, and decoded
data waiting for upload is limited to 512 MiB. The Walking Dead's first launch
lost about a second of render-thread hitches this way.

Decoded packed regions are cached beneath `process_config().cache_dir/textures`.
Keys include SHA-256 of the compressed content, format/color space, dimensions,
layer count, decoder revision and output mode. Output hashes detect corrupted
cache entries. Temporary files prevent partial reads, and cache I/O failures
fall back to decoding. There is currently no size-based eviction policy.

The fallback still needs conformance coverage for texture views, EGLImage aliases
of ASTC textures, and compressed/uncompressed `copy_image` format compatibility.
Those paths must not be used to infer full ASTC conformance. Normal game uploads
and sampling are the initial supported path. Native ANGLE ASTC decoding avoids
these HLE substitution limitations and is preferred when its build exposes it.

The next memory/performance step is a Vulkan compute decoder inserted at the
upload dependency, with precise subresource tracking and explicit staging-buffer
lifetime. Cache GPU-ready mip payloads using a decoder/shader revision. Preserve
sRGB RGB versus linear-alpha behavior. For guest Vulkan, the default ASTC decode
precision is FP16; this GLES RGBA8 fallback must not be reused blindly there.

BC7 transcoding is a separate, optional lossy storage policy. It can reduce the
4x to 36x expansion of ASTC-to-RGBA8, but needs quality comparisons, format/view
compatibility and upload-time measurements. Keep CPU astcenc as the correctness
reference and preserve original compressed assets. Neither GPU decoding nor BC7
transcoding is implemented by the present cache.
