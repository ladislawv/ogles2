#extension GL_OES_standard_derivatives : enable
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
// Boing 2026: the room. Neon grid antialiased with fwidth, soft shadow from
// a depth texture (12 rotated PCF taps, wider as the ball rises), the
// polished floor's reflection of the ball (render to texture) and of the
// back wall's grid (traced here), the ball's contact occlusion.
//   u_mode: x 1 = shadow, y 1 = reflection, z 1 = classic 1984 look,
//           w = output (0 screen, 1 bloom bright pass)
uniform vec3 u_light;
uniform vec3 u_lightcol;
uniform vec3 u_light2;    // the second light: its own colour and shadow
uniform vec3 u_light2col;
uniform vec3 u_eye;
uniform vec4 u_ball;      // centre, radius
uniform vec4 u_mode;
uniform vec2 u_inv_screen;
uniform float u_soft;     // PCF radius in shadow map coordinates
uniform vec3 u_occluder;  // the ball in the shadow map: centre, radius + u_soft
uniform float u_soft2;    // the same for the second light's shadow map
uniform vec3 u_occluder2;
uniform float u_backz;    // the back wall's z
uniform float u_ceil;     // the ceiling's y
uniform sampler2D u_shadow;
uniform sampler2D u_shadow2;
uniform sampler2D u_refl;
varying vec3 v_wpos;
varying vec3 v_n;
varying vec4 v_lpos;
varying vec4 v_lpos2;

const vec3 NEON = vec3(1.0, 0.10, 0.85);

// Grid lines: x the sharp line (about 1.5 pixels), y its glow (in cells,
// the same at every resolution).
vec2 grid(vec2 p)
{
    vec2 d = abs(fract(p - 0.5) - 0.5);
    vec2 px = d / max(fwidth(p), 1e-4);
    float line = clamp(1.3 - min(px.x, px.y), 0.0, 1.0);
    float glow = exp(-min(d.x, d.y) * 28.0);
    return vec2(line, glow);
}

// One light's shadow: its map, this point in the map's space, the ball's
// footprint there, the PCF radius and the number of taps (12 at most).
float shadow(sampler2D map, vec4 lpos, vec3 occluder, float soft, float taps)
{
    if (lpos.w <= 0.0 || u_mode.x < 0.5)
        return 1.0;
    vec3 s = lpos.xyz / lpos.w * 0.5 + 0.5;
    // Only the ball casts a shadow: away from its footprint in the map,
    // nothing to sample (most of the screen).
    vec2 fo = s.xy - occluder.xy;
    if (s.z > 1.0 || dot(fo, fo) > occluder.z * occluder.z)
        return 1.0;
    float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2832;
    vec2 o = vec2(cos(rot), sin(rot));
    mat2 golden = mat2(-0.7374, 0.6755, -0.6755, -0.7374); // 2.4 rad
    float lit = 0.0, z = s.z - 0.004;
    for (int i = 0; i < 12; i++) {
        if (float(i) >= taps)
            break;
        float r = sqrt((float(i) + 0.5) / taps) * soft;
        lit += step(z, texture2D(map, s.xy + o * r).r);
        o = golden * o;
    }
    return lit / taps;
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    if (u_mode.w > 2.5) {
        // M: the first light's shadow in red and blue, the second's in
        // green: each shows as the colour the other leaves.
        float a1 = shadow(u_shadow, v_lpos, u_occluder, u_soft, 12.0);
        float a2 = shadow(u_shadow2, v_lpos2, u_occluder2, u_soft2, 8.0);
        gl_FragColor = vec4(a1, a2, a1, 1.0);
        return;
    }
    vec3 n = normalize(v_n);
    vec2 uv = abs(n.y) > 0.5 ? v_wpos.xz : (abs(n.z) > 0.5 ? v_wpos.xy : v_wpos.zy);
    vec2 g = grid(uv);

    if (u_mode.z > 0.5) {
        // 1984: grey room, purple lines, a flat shadow.
        vec3 c = mix(vec3(0.66), vec3(0.62, 0.0, 0.62), g.x);
        c *= mix(0.45, 1.0, shadow(u_shadow, v_lpos, u_occluder, u_soft, 12.0));
        if (n.y < -0.5)
            c = vec3(0.66);
        gl_FragColor = vec4(c, 1.0);
        return;
    }

    vec3 vd = normalize(u_eye - v_wpos);
    vec3 tol = u_light - v_wpos;
    float dist2 = dot(tol, tol);
    vec3 l = tol * inversesqrt(dist2);
    float nl = max(dot(n, l), 0.0);
    // A spotlight from the ceiling panel, straight down; a little of it
    // washes the walls.
    float atten = 30.0 / (dist2 + 1.0) * (n.y > 0.5 ? smoothstep(0.3, 0.8, l.y) : 0.3);
    float sh = shadow(u_shadow, v_lpos, u_occluder, u_soft, 12.0);
    float sh2 = shadow(u_shadow2, v_lpos2, u_occluder2, u_soft2, 8.0);

    // The ball's occlusion of the sky above this point (analytic sphere).
    vec3 tb = u_ball.xyz - v_wpos;
    float db2 = dot(tb, tb);
    float occ = clamp(dot(n, tb) * inversesqrt(db2) * u_ball.w * u_ball.w / db2, 0.0, 1.0);
    float ao = 1.0 - occ * 0.9;

    // Dark lacquer; the floor catches a pool of light that the shadow cuts.
    vec3 albedo = n.y > 0.5 ? vec3(0.06, 0.055, 0.07) : vec3(0.05, 0.035, 0.08);
    vec3 col = albedo * (u_lightcol * nl * atten * sh + vec3(0.05, 0.02, 0.10) * ao);
    col += vec3(1.0, 0.2, 0.2) * occ * 0.10 * sh; // red light bounced off the ball

    // The second light: a pool of its colour where it passes.
    vec3 t2 = u_light2 - v_wpos;
    float d2 = dot(t2, t2);
    vec3 l2 = t2 * inversesqrt(d2);
    float nl2 = max(dot(n, l2), 0.0), at2 = 14.0 / (d2 + 1.0);
    col += albedo * u_light2col * nl2 * at2 * ao * sh2;
    col += NEON * (g.x * 1.25 + g.y * 0.30) * mix(0.12, 1.0, sh) * mix(0.35, 1.0, sh2) * ao;

    // Ceiling: the light panel.
    if (n.y < -0.5) {
        vec2 q = abs(v_wpos.xz - u_light.xz);
        float panel = (1.0 - smoothstep(1.4, 1.5, q.x)) * (1.0 - smoothstep(0.7, 0.8, q.y));
        col = mix(col, u_lightcol * 1.4, panel);
    }

    if (n.y > 0.5) {
        // Polished floor: a sharp highlight and the reflections.
        vec3 h = normalize(l + vd);
        float a = 0.08 * 0.08, nh = max(dot(n, h), 0.0);
        float d = nh * nh * (a - 1.0) + 1.0;
        col += u_lightcol * a / (3.14159 * d * d) * 0.06 * nl * atten * sh;
        vec3 h2 = normalize(l2 + vd);
        float nh2 = max(dot(n, h2), 0.0), dh2 = nh2 * nh2 * (a - 1.0) + 1.0;
        col += u_light2col * a / (3.14159 * dh2 * dh2) * 0.06 * nl2 * at2 * sh2;

        float nv = max(dot(n, vd), 0.0);
        float fres = mix(0.30, 1.0, pow(1.0 - nv, 5.0)) * 0.85;
        // The back wall's grid and the ceiling's light panel, by tracing
        // the mirrored view ray to them.
        vec3 r = reflect(-vd, n);
        {
            vec3 c = v_wpos + r * ((u_ceil - v_wpos.y) / r.y);
            vec2 q = abs(c.xz - u_light.xz);
            float panel = (1.0 - smoothstep(1.3, 1.6, q.x)) * (1.0 - smoothstep(0.6, 0.9, q.y));
            col += u_lightcol * panel * fres * 1.2 * sh;
        }
        if (r.z < -0.01) {
            float t = (u_backz - v_wpos.z) / r.z;
            vec3 hit = v_wpos + r * t;
            if (hit.y < 7.0) {
                vec2 gw = grid(hit.xy);
                col += NEON * (gw.x * 1.1 + gw.y * 0.25) * fres * exp(-t * 0.08);
            }
        }
        if (u_mode.y > 0.5) {
            // One bilinear read of the half-size texture: soft enough.
            vec4 rc = texture2D(u_refl, gl_FragCoord.xy * u_inv_screen);
            col = col * (1.0 - rc.a * fres * 0.8) + rc.rgb * 4.0 * fres;
        }
    }

    // Light fades into the depth of the room.
    col *= exp(-max(length(v_wpos - u_eye) - 8.0, 0.0) * 0.07);

    if (u_mode.w > 0.5)
        gl_FragColor = vec4(max(col - 1.0, 0.0) * 0.25, 1.0);
    else
        gl_FragColor = vec4(pow(aces(col), vec3(1.0 / 2.2)), 1.0);
}
