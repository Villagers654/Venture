#version 450
// One composition layer drawn into one eye of a projection layer, for a layer
// the host cannot compose itself: a sphere (equirect), a cylinder, a cube or a
// plane given by a VrApi tan-angle matrix. Each pixel's ray is taken into the
// layer's shape space, where the shapes are canonical (a unit cylinder about
// +Y, a unit sphere, cube directions), and the hit is mapped to the source.
layout(set=0,binding=0) uniform sampler2DArray sourceImage;
layout(push_constant) uniform Layer {
  // Pixel (x, y, 1, w) to shape space; w = 1 for the eye's position and the
  // pixel's point, 0 for its direction. A plane's rows give (S, T, Q) * Q', row3 Q'.
  vec4 row0, row1, row2, row3;
  vec4 mapU;   // the shape's (a, b): u = x*a + y*b + z; w: v's a coefficient
  vec4 mapV;   // x: v's b coefficient, y: v's constant; zw: the sampled rect's minimum
  vec4 clip;   // xy: the sampled rect's maximum; z: shape + 8 * source layer;
               // w: source factor + 8 * destination factor (0 zero, 1 one, 2 alpha, 5 1 - alpha)
  uvec4 color; // half floats: color scale rg, ba, then color bias rg, ba
} p;
layout(location=0) out vec4 color;
const float PI = 3.14159265358979;
const int PLANE = 0, CYLINDER = 1, SPHERE = 2, FAR_SPHERE = 3, CUBE = 4;
float factor(int f, float alpha) {
  return f == 0 ? 0.0 : f == 1 ? 1.0 : f == 2 ? alpha : 1.0 - alpha;
}
bool place(vec2 ab, out vec2 uv) {
  uv = vec2(dot(p.mapU.xyz, vec3(ab, 1.0)), dot(vec3(p.mapU.w, p.mapV.xy), vec3(ab, 1.0)));
  return all(greaterThanEqual(uv, p.mapV.zw)) && all(lessThanEqual(uv, p.clip.xy));
}
// An angle about +Y (a) is periodic: try the turns either side too.
bool place_turn(vec2 ab, out vec2 uv) {
  return place(ab, uv) || place(ab - vec2(2.0 * PI, 0.0), uv) || place(ab + vec2(2.0 * PI, 0.0), uv);
}
void main() {
  color = vec4(0);
  int code = int(p.clip.z + 0.5), blend = int(p.clip.w + 0.5);
  int shape = code & 7;
  float layer = float(code >> 3);
  vec4 direction = vec4(gl_FragCoord.xy, 1.0, 0.0), point = vec4(gl_FragCoord.xy, 1.0, 1.0);
  vec3 d = vec3(dot(p.row0, direction), dot(p.row1, direction), dot(p.row2, direction));
  vec2 uv;
  if (shape == PLANE) {
    // Q' > 0: in front of the eye; Q > 0: the projective map's front half.
    if (dot(p.row3, direction) <= 0.0 || d.z <= 0.0 || !place(d.xy / d.z, uv)) return;
  } else if (shape == CYLINDER || shape == SPHERE) {
    vec4 o4 = vec4(p.row0.w, p.row1.w, p.row2.w, p.row3.w);
    vec4 q4 = vec4(dot(p.row0, point), dot(p.row1, point), dot(p.row2, point), dot(p.row3, point));
    if (o4.w <= 0.0 || q4.w <= 0.0) return;
    vec3 o = o4.xyz / o4.w, r = q4.xyz / q4.w - o;
    // Where the ray meets the surface: the nearer hit in front, else the farther.
    float a, b, c;
    if (shape == CYLINDER) {
      a = dot(r.xz, r.xz); b = dot(o.xz, r.xz); c = dot(o.xz, o.xz) - 1.0;
    } else {
      a = dot(r, r); b = dot(o, r); c = dot(o, o) - 1.0;
    }
    float disc = b * b - a * c;
    if (a <= 0.0 || disc < 0.0) return;
    float root = sqrt(disc);
    bool found = false;
    for (int i = 0; i < 2 && !found; ++i) {
      float t = (-b + (i == 0 ? -root : root)) / a;
      if (t <= 0.0) continue;
      vec3 s = o + t * r;
      vec2 ab = shape == CYLINDER ? vec2(atan(s.x, -s.z), s.y)
                                  : vec2(atan(s.x, -s.z), asin(clamp(s.y / length(s), -1.0, 1.0)));
      found = place_turn(ab, uv);
    }
    if (!found) return;
  } else if (shape == FAR_SPHERE) {
    vec3 n = normalize(d);
    if (!place_turn(vec2(atan(n.x, -n.z), asin(clamp(n.y, -1.0, 1.0))), uv)) return;
  } else if (shape == CUBE) {
    // Vulkan's (and GL's) cube face selection: faces +X -X +Y -Y +Z -Z.
    vec3 m = abs(d);
    float sc, tc, ma;
    int face;
    if (m.x >= m.y && m.x >= m.z) { face = d.x > 0.0 ? 0 : 1; ma = m.x; sc = d.x > 0.0 ? -d.z : d.z; tc = -d.y; }
    else if (m.y >= m.z) { face = d.y > 0.0 ? 2 : 3; ma = m.y; sc = d.x; tc = d.y > 0.0 ? d.z : -d.z; }
    else { face = d.z > 0.0 ? 4 : 5; ma = m.z; sc = d.z > 0.0 ? d.x : -d.x; tc = -d.y; }
    if (!(ma > 0.0)) return;
    uv = 0.5 * vec2(sc / ma + 1.0, tc / ma + 1.0);
    layer += float(face);
  } else {
    return;
  }
  vec4 value = textureLod(sourceImage, vec3(uv, layer), 0.0) *
      vec4(unpackHalf2x16(p.color.x), unpackHalf2x16(p.color.y)) +
      vec4(unpackHalf2x16(p.color.z), unpackHalf2x16(p.color.w));
  // Composited premultiplied (ONE, ONE_MINUS_SRC_ALPHA) over the layers below:
  // src * S + dst * D is color src.rgb * S with alpha 1 - D.
  color = vec4(value.rgb * factor(blend & 7, value.a), 1.0 - factor(blend >> 3, value.a));
}
