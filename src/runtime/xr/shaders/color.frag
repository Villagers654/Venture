#version 450
#ifdef MULTISAMPLE
layout(set=0,binding=0) uniform sampler2DMS sourceImage;
#else
layout(set=0,binding=0) uniform sampler2D sourceImage;
#endif
#ifdef PASSTHROUGH_ALPHA
layout(push_constant) uniform Transform { vec4 row0; vec4 row1; vec4 row2; vec4 key; } transform;
#else
layout(push_constant) uniform Transform { vec4 row0; vec4 row1; vec4 row2; } transform;
#endif
layout(location=0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
#ifdef MULTISAMPLE
  vec4 value = vec4(0);
  int samples = textureSamples(sourceImage);
  for (int i=0; i<samples; ++i) value += texelFetch(sourceImage,p,i);
  value /= float(samples);
#else
  vec4 value = texelFetch(sourceImage,p,0);
#endif
  // sRGB image views decode before this linear-light transform; the same
  // sRGB attachment format encodes the output. UNORM/float remain linear.
  color = vec4(dot(transform.row0.rgb,value.rgb), dot(transform.row1.rgb,value.rgb),
               dot(transform.row2.rgb,value.rgb), value.a);
#ifdef PASSTHROUGH_ALPHA
  if (transform.row0.w != 0) color.rgb *= value.a;
  if (transform.row1.w != 0) color.a = 1;
  // Reconstruction opacity P over the black environment: E stays unchanged,
  // A'=1-P*(1-A) yields E + P*(1-A)*camera on the headset.
  if (transform.row2.w != 1) color.a = 1 - transform.row2.w * (1 - color.a);
  // Color key: a hard key at coverage one half, never blended, so the key stays
  // exact for any keyer tolerance and edges carry no key fringe. Elsewhere the
  // straight colour is shown opaque (coverage is at least one half there unless
  // the opacity is below 1, hence the floor).
  if (transform.key.w != 0) {
    float coverage = transform.row1.w != 0 ? 1.0 : value.a;
    color = color.a < 0.5 ? vec4(transform.key.rgb, 1)
                          : vec4(min(color.rgb / max(coverage, 0.5), vec3(1)), 1);
  }
#endif
}
