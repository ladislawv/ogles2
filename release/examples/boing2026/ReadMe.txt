Boing 2026
==========

The Amiga's Boing Ball of 1984, remade forty-two years on for ogles2.library:
OpenGL ES 2.0 with programmable shaders, drawn by the Raspberry Pi 4's GPU
(VideoCore VI, V3D 4.2) in a PiStorm under Emu68.

A glossy checkered ball bounces through a neon-gridded room. It squashes on
the floor and the walls, casts a soft shadow, is mirrored in the polished
floor, its highlight blooms, and every edge is antialiased. Each of those is
something the 1984 demo could only fake, and a fixed-function pipeline cannot
do at all: this needs shaders.


Running it
----------
Needs ogles2.library 1.3 (in LIBS:, or in libs/ beside the program) and a
32-bit RTG screen. The shaders come compiled (shaders/), so the shader
compiler is not needed.

    boing2026 [WIDTH=w] [HEIGHT=h] [FRAMES=n] [NOMSAA] [NOSOUND] [FULLSCREEN]
              [TIMING] [SHOWMAP] [VSYNC] [TOUR] [SCALE=percent]

  TOUR       switches the effects off and on one by one, every 5 seconds, and
             says in the window's title what each one proves. Start here.
  FULLSCREEN a screen of WIDTH x HEIGHT (640 x 480) of its own
  TIMING     prints the time each pass takes, every 120 frames
  SHOWMAP    starts with the shadow map shown (key M)
  VSYNC      waits for the display before each frame (no tearing); without
             it the frame rate is not limited
  SCALE      the size the room is drawn at, in percent of the screen's,
             before it is brought up to it (the ball is always drawn at full
             size). By default 100 up to 640 x 480; above, about 78% of
             640 x 480's worth of pixels: 50 at 1280 x 720.
  NOMSAA     without the 4x antialiasing
  NOSOUND    without the boing (synthesised at start, played on Paula)

Keys: 1 shadow  2 reflection  3 bloom  4 per-pixel / per-vertex light
      5 checker antialiasing  6 the 1984 look  M shadow map  T time the passes
      L stop or start the lights flying round the ball  Space pause  Esc quit.
      Drag with the left mouse button to move the camera.


What each effect proves
-----------------------
Measured on a Raspberry Pi 4 in an A1200's PiStorm, full screen, 4x MSAA,
frames not limited (no VSYNC):

    640 x 480    room at 100%   60 frames a second
    1280 x 720   room at 50%    60 frames a second
    1280 x 720   room at 100%   about 28 frames a second (SCALE=100)

Times below are per frame at 640 x 480, with the GPU finishing each pass
(TIMING).

1. The checkerboard, per pixel (key 5)
   Proves: a pattern worked out in the fragment shader from the sphere's own
   coordinates, with no texture, sharp at any distance. fwidth()
   (OES_standard_derivatives) blends each edge over exactly one pixel. Without
   it the edges are jagged and crawl as the ball turns.
   Pi 4: a few dozen instructions, only where the ball covers the screen.
   Watch for: the seam where atan() wraps (taken care of by a second, shifted
   copy of the coordinate) and the poles, where the cells shrink to nothing
   and fade to grey instead of shimmering.

2. Per-pixel light (key 4)
   Proves: a GGX clear-coat highlight and the reflection of the room's light
   panel, computed for every pixel, from two point lights flying round the
   ball: a white one and a coloured one, each casting its own shadow. The
   second light's colour runs slowly round the colour wheel. Lit per vertex instead (Gouraud, as a
   fixed-function pipeline does), the highlight smears across the triangles
   and wobbles as the ball turns.
   Pi 4: the ball's fragment shader is 416 instructions; the whole ball pass
   costs about as much as the room behind it does per pixel.
   Watch for: precision. The shaders ask for highp where the GPU has it
   (V3D does: 32-bit floats throughout).

3. The soft shadows (keys 1 and M)
   Proves: render to depth textures (OES_depth_texture): the ball drawn from
   each light into its own 512 x 512 shadow map, then rotated PCF taps in the
   room's shader (12 for the white light, 8 for the cyan one). Each shadow
   takes away only its own light's colour, so where one falls the other
   light still tints it. The penumbras widen as the ball rises. The 1984
   demo drew one flat shape; key 6 shows it. M shows both shadow terms:
   the white light's in magenta, the cyan light's in green.
   Pi 4: 1.2 ms to draw both maps. The taps cost only inside the ball's
   footprint in the map, worked out on the CPU each frame: everywhere else the
   shader skips them, which took the main pass from 12.3 to 8.2 ms.
   Watch for: depth textures are sampled nearest (no hardware comparison in
   GLES2), so the softness is all in the taps; bias against acne; a program
   that samples depth needs its .v3dw file beside the .v3dp (shipped).

4. The polished floor (key 2)
   Proves: render to texture. The ball is drawn upside down, mirrored in the
   floor, into a half-size texture, which the floor reads by screen position
   and blends by Fresnel. The back wall's grid and the ceiling's light panel
   are mirrored too, by tracing the reflected ray to them in the shader.
   Pi 4: 0.7 ms.
   Watch for: the mirror turns the triangles round (glFrontFace(GL_CW) for
   that pass), and framebuffer objects have no MSAA, so the reflection is
   softened on purpose.

5. Bloom (key 3)
   Proves: several passes in a chain. What is brighter than white (the
   highlight, the light panel, the neon) is drawn at a quarter of the size,
   blurred twice each way with a 9-tap Gaussian in 5 bilinear reads, and
   added to the scene with the vignette as the room is brought up to the
   screen; the ball, drawn after, adds the bloom under itself in its own
   shader.
   Pi 4: 2.3 ms for the bright pass and the blurs; adding it costs nothing
   extra, being part of the pass that brings the room to the screen.
   Watch for: 8-bit render targets clip at 1.0, so the bright pass stores a
   quarter of the value and the composite multiplies it back.

   The second light shows itself as a lens flare: a hot core with a
   four-point star, a halo and coloured ghosts along the line from it through
   the middle of the screen, added to the window after the ball. It fades
   out when the ball passes in front of the light (a ray-against-sphere test
   on the CPU, so no GPU read-back) and as the light leaves the screen.
   Pi 4: eight small quads, under 0.3 ms.

6. 4x MSAA
   Proves: smooth silhouettes on the ball and the room's edges.
   Pi 4: nearly free: the GPU resolves the samples in its tile memory.
   Watch for: only the window's framebuffer is multisampled. The room goes
   through a texture (for the render scale) and has no MSAA, but its edges
   are the walls' corners and its lines are antialiased in the shader; the
   ball, whose silhouette needs it, is drawn into the window itself.


How it is put together
----------------------
Five passes a frame: the shadow map; the mirrored ball; the bloom (bright
pass, then blurs); the room, at the render scale; the window: the room
brought up to its size with the bloom and the vignette in one opaque pass,
then the ball over it. The room's shader is what costs (it grows with the
pixels); drawing it at a fraction of a large screen is what makes 720p run
at 60. The room is a
box seen from inside and the ball is convex, so no pass but the shadow map
needs a depth buffer: the window has none. Each pass that draws into a
texture is cleared first, so the GPU does not load the old contents into its
tiles.

The shaders: ball (fragment about 430 instructions), room (about 1,050, of
which the shadow taps run only near the ball), blur (51), composite (about
35), shadow map (6).
For comparison, Boing Ocean's single fragment shader of about 1,300
instructions runs at 60 frames a second at 640 x 480.

With VSYNC, the frame rate is held at the display's (50 or 60).


Building
--------
In WSL with bebbo's amiga-gcc (as ogles2 uses: TC=~/amiga-gcc-install) and,
for the compiled shaders, the offline compiler of the ogles2 sources:

    make          boing2026
    make shaders  build/v3dp/<hash>.v3dp
    make dist     dist/Boing2026/

The shaders' sources are in shaders/; boing2026_shaders.h is made from them.
A changed shader needs new binaries (make shaders).


MIT License. The Boing Ball is the Amiga's, by Dale Luck and R.J. Mical,
1984.
