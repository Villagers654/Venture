#version 450
#ifdef MULTISAMPLE
layout(set=0,binding=0) uniform sampler2DMS sourceImage;
#else
layout(set=0,binding=0) uniform sampler2D sourceImage;
#endif
layout(push_constant) uniform Transform { vec4 row0; vec4 row1; vec4 row2; } transform;
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
}
