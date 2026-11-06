The ASTC decoder and lookup tables, and ETC2/EAC routines, are adapted from
Google gfxstream, revision `07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe`,
`host/vulkan/emulated_textures/shaders/`. Copyright The Android Open Source
Project, licensed under Apache-2.0; see LICENSE.gfxstream.

QuestPort modifications: buffer input/output, bounds and illegal-block checks,
16-bit endpoint interpolation, sRGB rounding, Vulkan FP16 output, and dispatch
parameters for partial, strided, layered uploads. No BC recompression is used.

The UNORM16-to-FP16 conversion follows Arm astcenc's `unorm16_to_sf16`, from
revision `18b20e6641503c41b2aac55664a2400534ba7212`, `Source/astcenc_vecmathlib.h`.
The HDR endpoint modes, LNS-to-FP16 conversion, HDR void extents, and the
6-level minimum endpoint range are ported from Arm astcenc revision
`30aabb3f42406df45a910d8496f9bee17eeba9bb` (`Source/astcenc_color_unquantize.cpp`,
`astcenc_vecmathlib.h`, `astcenc_symbolic_physical.cpp`). Copyright Arm Limited,
Apache-2.0 (same license text).
