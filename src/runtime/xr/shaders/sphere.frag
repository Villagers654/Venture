#version 450
// One equirect layer (XR_KHR_composition_layer_equirect or _equirect2) drawn
// into one eye of a projection layer, for hosts without those layer types.
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform Sphere {
  vec4 row0, row1, row2;  // eye-to-layer rotation rows; w: the eye's position in layer space
  vec4 tangents;          // the eye's tan(angleLeft), tan(angleRight), tan(angleUp), tan(angleDown)
  vec4 mapping;           // equirect2: centralHorizontal, upperVertical, lowerVertical angles; w: radius
                          // equirect: scale.xy, bias.xy
  vec4 rect;              // the subimage: offset.xy and size.zw in normalized image coordinates
  vec4 mode;              // x: 1 equirect (v1), y: radius (v1), z: 0 opaque, 1 premultiplied, 2 unpremultiplied
  vec4 extent;            // xy: the eye's viewport size in pixels
} p;
layout(location=0) out vec4 color;
const float PI = 3.14159265358979;
void main() {
  vec2 at = gl_FragCoord.xy / p.extent.xy;
  // Rows run from the top of the view (up) to its bottom (down).
  vec3 ray = normalize(vec3(mix(p.tangents.x, p.tangents.y, at.x), mix(p.tangents.z, p.tangents.w, at.y), -1.0));
  vec3 d = normalize(vec3(dot(p.row0.xyz, ray), dot(p.row1.xyz, ray), dot(p.row2.xyz, ray)));
  vec3 eye = vec3(p.row0.w, p.row1.w, p.row2.w);
  float radius = p.mode.x > 0.5 ? p.mode.y : p.mapping.w;
  if (radius > 0.0) {
    // A finite sphere around the layer's pose: where the ray leaves it.
    float b = dot(eye, d), c = dot(eye, eye) - radius * radius, disc = b * b - c;
    if (disc < 0.0) { color = vec4(0); return; }
    float t = -b + sqrt(disc);
    if (t <= 0.0) { color = vec4(0); return; }
    d = normalize(eye + t * d);
  }
  float lon = atan(d.x, -d.z), lat = asin(clamp(d.y, -1.0, 1.0));
  vec2 uv;
  if (p.mode.x > 0.5) {
    uv = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI) * p.mapping.xy + p.mapping.zw;
  } else {
    float central = p.mapping.x, upper = p.mapping.y, lower = p.mapping.z;
    uv = vec2(central >= 2.0 * PI ? lon / (2.0 * PI) + 0.5 : lon / central + 0.5, (upper - lat) / (upper - lower));
  }
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { color = vec4(0); return; }
  vec4 value = textureLod(sourceImage, p.rect.xy + uv * p.rect.zw, 0.0);
  // Composited premultiplied (ONE, ONE_MINUS_SRC_ALPHA) over the layers below.
  if (p.mode.z < 0.5) color = vec4(value.rgb, 1.0);
  else if (p.mode.z < 1.5) color = value;
  else color = vec4(value.rgb * value.a, value.a);
}
