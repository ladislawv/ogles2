/*
 * sdl2_sprites: SDL2's 2D API drawn by the GPU through SDL's GLES2 renderer
 * on ogles2.library. A texture made in memory, 64 copies rotated, scaled and
 * blended each frame. No GL code: the renderer does it.
 *   usage: sdl2_sprites [seconds]     (Esc or the close gadget quits)
 * Prints the frame rate at the end. The renderer writes its shaders at run
 * time: the first run needs the shader compiler (LIBS:v3dglslc) and caches
 * them in OGLES2Cache: (or T:).
 *
 * MIT licence (the ogles2 project's LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>

long __stack = 65536; /* clib2: the program's stack */
static const char version[] __attribute__((used)) = "$VER: sdl2_sprites 1.0 (16.10.2026)";

#define W 640
#define H 480
#define N 64

static SDL_Texture *make_texture(SDL_Renderer *r)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture *t;
    int x, y;
    if (!s)
        return NULL;
    for (y = 0; y < 64; y++) {
        Uint8 *p = (Uint8 *)s->pixels + y * s->pitch;
        for (x = 0; x < 64; x++, p += 4) {
            int dx = x - 32, dy = y - 32, d2 = dx * dx + dy * dy;
            p[0] = (Uint8)(x * 4);
            p[1] = (Uint8)(y * 4);
            p[2] = ((x ^ y) & 8) ? 255 : 96;
            p[3] = d2 < 31 * 31 ? 255 : 0; /* a disc: blending shows the edge */
        }
    }
    t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (t)
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

int main(int argc, char **argv)
{
    SDL_Window *win;
    SDL_Renderer *r;
    SDL_Texture *tex;
    SDL_RendererInfo info;
    Uint32 start, now, limit = argc > 1 ? (Uint32)atoi(argv[1]) * 1000 : 0;
    unsigned frames = 0;
    int running = 1, i;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 20;
    }
    /* The GPU renderer by name; without the hint SDL takes the first that works. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    win = SDL_CreateWindow("sdl2_sprites", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
    r = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    tex = r ? make_texture(r) : NULL;
    if (!tex) {
        printf("sdl2_sprites: %s\n", SDL_GetError());
        SDL_Quit();
        return 20;
    }
    SDL_GetRendererInfo(r, &info);
    printf("renderer: %s\n", info.name);

    start = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        float t;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                running = 0;
        }
        now = SDL_GetTicks();
        if (limit && now - start >= limit)
            running = 0;
        t = (now - start) / 1000.0f;

        SDL_SetRenderDrawColor(r, 16, 16, 40, 255);
        SDL_RenderClear(r);
        for (i = 0; i < N; i++) {
            float a = t * 0.7f + i * (6.2831853f / N);
            int size = 32 + (i % 8) * 8;
            SDL_Rect dst;
            dst.w = dst.h = size;
            dst.x = W / 2 + (int)(SDL_cosf(a) * (60 + i * 3)) - size / 2;
            dst.y = H / 2 + (int)(SDL_sinf(a) * (40 + i * 2.5f)) - size / 2;
            SDL_RenderCopyEx(r, tex, NULL, &dst, t * 90.0 + i * 20, NULL, SDL_FLIP_NONE);
        }
        SDL_RenderPresent(r);
        frames++;
    }
    now = SDL_GetTicks();
    if (now > start)
        printf("%u frames in %.1f s: %.1f fps\n", frames, (now - start) / 1000.0, frames * 1000.0 / (now - start));

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
