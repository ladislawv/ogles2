#extension GL_OES_standard_derivatives : enable
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
// Boing 2026: the ball. The checkerboard is worked out per pixel from the
// sphere's own coordinates (no texture) and antialiased with fwidth; the
// clear-coat is GGX, the room it mirrors is procedural.
//   u_mode: x 1 = Gouraud lighting (per vertex), y 1 = antialiased checker,
//           z 1 = classic 1984 look, w = output (0 screen, 1 bloom bright
//           pass, 2 linear / 4 for the floor's reflection)
uniform vec3 u_light;
uniform vec3 u_lightcol;
uniform vec3 u_light2;    // the second light: its own colour and shadow
uniform vec3 u_light2col;
uniform vec3 u_eye;
uniform vec4 u_mode;
uniform sampler2D u_bloomtex;
uniform vec4 u_post;      // 1 / window size, bloom strength, aspect
varying vec3 v_obj;
varying vec3 v_wpos;
varying vec3 v_n;
varying vec2 v_gour;

const float PI = 3.14159265;

// One axis of the checkerboard, -1 or 1, blended over a pixel at the edges.
float square(float a, float fw)
{
    float t = abs(fract(a * 0.5) - 0.5) * 2.0 - 0.5;
    if (u_mode.y < 0.5)
        return t < 0.0 ? -1.0 : 1.0;
    return clamp(t / max(fw, 1e-4), -0.5, 0.5) * 2.0;
}

// The room as the ball sees it: a dark purple box, its neon horizon and the
// ceiling's light panel.
vec3 env(vec3 r)
{
    vec3 l = normalize(u_light - v_wpos);
    vec3 c = mix(vec3(0.012, 0.004, 0.03), vec3(0.30, 0.03, 0.42), exp(-abs(r.y) * 4.0));
    if (r.y < 0.0) {
        vec2 g = r.xz / max(-r.y, 0.05) * 2.0;
        vec2 d = abs(fract(g) - 0.5);
        c += vec3(0.9, 0.08, 0.8) * exp(-min(0.5 - d.x, 0.5 - d.y) * 10.0) * exp(r.y * 3.0) * 0.3;
    }
    float p = dot(r, l);
    c += u_lightcol * (smoothstep(0.97, 0.985, p) * 9.0 + pow(max(p, 0.0), 24.0) * 0.15);
    return c;
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec3 p = normalize(v_obj);
    float u = atan(p.z, p.x) * (8.0 / PI);
    float v = asin(clamp(p.y, -1.0, 1.0)) * (8.0 / PI);
    float u2 = fract(u / 16.0) * 16.0; // the same, its seam at u = 0, not +-8
    float fu = min(fwidth(u), fwidth(u2)), fv = fwidth(v);
    float k = 0.5 + 0.5 * square(u, fu) * square(v + 4.0, fv);

    vec3 red = vec3(0.80, 0.015, 0.01), white = vec3(0.92, 0.90, 0.88);
    if (u_mode.z > 0.5) {
        gl_FragColor = vec4(mix(white, vec3(0.95, 0.0, 0.0), k), 1.0);
        return;
    }
    vec3 base = mix(white, red, k);

    vec3 n = normalize(v_n);
    vec3 vd = normalize(u_eye - v_wpos);
    vec3 l = normalize(u_light - v_wpos);
    vec3 h = normalize(l + vd);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, vd), 1e-3);
    float fres = 0.04 + 0.96 * pow(1.0 - nv, 5.0);

    float ao = mix(0.35, 1.0, clamp(v_wpos.y * 1.4, 0.0, 1.0)) * mix(1.0, 0.8, max(-n.y, 0.0));
    vec3 hemi = mix(vec3(0.10, 0.02, 0.14), vec3(0.06, 0.05, 0.12), n.y * 0.5 + 0.5);
    vec3 col;
    if (u_mode.x > 0.5) {
        // Per vertex: the highlight smears between the vertices.
        col = base * (u_lightcol * v_gour.x * 0.30 + hemi * ao) + u_lightcol * v_gour.y * 0.8;
    } else {
        float a = 0.16 * 0.16, nh = max(dot(n, h), 0.0), lh = max(dot(l, h), 0.0);
        float d = nh * nh * (a - 1.0) + 1.0;
        float spec = a / (PI * d * d) * (0.04 + 0.96 * pow(1.0 - lh, 5.0)) / (4.0 * lh * lh + 1e-3);
        col = base * (u_lightcol * nl * 0.30 + hemi * ao);
        col += u_lightcol * spec * nl * 3.0;
        col += env(reflect(-vd, n)) * fres * ao;
        col += vec3(0.7, 0.1, 1.0) * pow(1.0 - nv, 4.0) * 0.35; // neon rim

        // The second light: diffuse and its own highlight, fading with
        // distance.
        vec3 t2 = u_light2 - v_wpos;
        float d2 = dot(t2, t2);
        vec3 l2 = t2 * inversesqrt(d2);
        vec3 h2 = normalize(l2 + vd);
        float nl2 = max(dot(n, l2), 0.0), nh2 = max(dot(n, h2), 0.0), lh2 = max(dot(l2, h2), 0.0);
        float dd2 = nh2 * nh2 * (a - 1.0) + 1.0;
        float spec2 = a / (PI * dd2 * dd2) * (0.04 + 0.96 * pow(1.0 - lh2, 5.0)) / (4.0 * lh2 * lh2 + 1e-3);
        col += u_light2col * (12.0 / (d2 + 2.0)) * nl2 * (base * 0.30 + spec2 * 3.0);
    }

    if (u_mode.w > 1.5)
        gl_FragColor = vec4(col * 0.25, 1.0);
    else if (u_mode.w > 0.5)
        gl_FragColor = vec4(max(col - 1.0, 0.0) * 0.25, 1.0);
    else {
        // On screen: the bloom and the vignette, as composite.frag gives the
        // room around it.
        vec2 suv = gl_FragCoord.xy * u_post.xy;
        vec3 b = texture2D(u_bloomtex, suv).rgb * 4.0 * u_post.z;
        vec2 q = (suv - 0.5) * vec2(u_post.w, 1.0);
        float vig = 1.0 - dot(q, q) * 0.55;
        gl_FragColor = vec4(1.0 - exp(-b) + pow(aces(col), vec3(1.0 / 2.2)) * vig, 1.0);
    }
}
