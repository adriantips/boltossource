/* boltcraft -- a voxel sandbox for BoltOS, built with the SDK in sdk/.
 *
 * Nothing here is special to this program: SYS_FBINFO claims the panel,
 * SYS_FBPRESENT blits a frame, SYS_GETKEY reads the keyboard, and that is the
 * whole of the platform. No window, no toolkit, no GPU.
 *
 * A note on the controls, because they look eccentric until you know why.
 * SYS_GETKEY hands back characters, not key up/down, and the xHCI HID path in
 * drivers/xhci.c drops a scancode that was already down in the last report --
 * so a held key produces exactly one event and there is no way to ask whether
 * W is still pressed. Movement is therefore impulse-based: a tap gives you a
 * short glide that eases out, tapping repeatedly runs, and R latches autorun
 * for when you just want to cross a continent. If your keyboard does repeat,
 * the same model turns into ordinary smooth WASD for free.
 */
#include "craft.h"

#define K_UP     0x11
#define K_DOWN   0x12
#define K_LEFT   0x13
#define K_RIGHT  0x14
#define K_ESC    27

#define EYE      1.62f
#define HALFW    0.30f
#define BODY     1.78f

#define SAVE_PATH "/home/user/boltcraft.wld"

static struct camera cam;
static float pfx, pfy, pfz;             /* feet */
static float vx, vy, vz;
static float tvel, pvel;                /* look velocities */
static int   fly, onground, autorun;

static float phase = 1.15f;             /* mid-morning */
static int   clock_running = 1;

static const uint8_t bar[10] = {
    B_GRASS, B_DIRT, B_STONE, B_COBBLE, B_SAND,
    B_LOG, B_LEAVES, B_PLANK, B_GLASS, B_BRICK
};
static int slot;

static char  msg[64];
static float msg_left;

static uint32_t *fbbuf, *rbuf;
static struct user_fbinfo FB;
static int show_hud = 1;
static int auto_detail = 1;

/* ------------------------------------------------------------------ time -- */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

/* BoltOS has no clock syscall, so the only way to a real dt is to ask the CPU
 * how fast its own counter runs. CR4.TSD is never set (kernel/main.c touches
 * SMEP and UMIP and nothing else), so rdtsc and cpuid both work from ring 3.
 * Any of the three leaves may answer 0, in which case we fake a fixed step and
 * the game runs at whatever pace the machine manages. */
static uint64_t tsc_hz(void) {
    uint32_t a, b, c, d;

    cpuid(0x40000000u, 0, &a, &b, &c, &d);
    if (a >= 0x40000010u) {
        cpuid(0x40000010u, 0, &a, &b, &c, &d);
        if (a > 100000u) return (uint64_t)a * 1000ull;    /* kHz, per the KVM/VMware leaf */
    }
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maxleaf = a;
    if (maxleaf >= 0x15u) {
        cpuid(0x15u, 0, &a, &b, &c, &d);
        if (a && b && c) return (uint64_t)c * b / a;
    }
    if (maxleaf >= 0x16u) {
        cpuid(0x16u, 0, &a, &b, &c, &d);
        if ((a & 0xFFFFu) > 100u) return (uint64_t)(a & 0xFFFFu) * 1000000ull;
    }
    return 0;
}

/* CPUID answers 0 to all of the above under QEMU's interpreter, which is
 * exactly where knowing the frame time matters most -- without it the detail
 * setting cannot adapt and the game sits at 1 fps insisting everything is
 * fine. So: fall back to the one millisecond-accurate wait the ABI does
 * publish. SYS_ACCEPT's timeout is counted in PIT ticks by net/tcp.c, so an
 * accept on a port nobody will connect to is a sleep of a known length, and
 * two of different lengths give the rate without the fixed overhead of the
 * call. Costs half a second of startup and needs no network traffic.
 *
 * (First attempt used SYS_CONNECT to a black-hole address, which returns
 * immediately when the stack has no route -- so it measured nothing and
 * claimed a 4 THz CPU.) */
static uint64_t tsc_hz_by_accept(void) {
    int fd = socket(AF_INET, SOCK_STREAM);
    if (fd < 0) return 0;
    if (listen_port(fd, 47311) != 0) { close(fd); return 0; }

    uint32_t ip; uint16_t pt;
    uint64_t t0 = rdtsc();
    accept_conn(fd, &ip, &pt, 120);
    uint64_t t1 = rdtsc();
    accept_conn(fd, &ip, &pt, 420);
    uint64_t t2 = rdtsc();
    close(fd);

    uint64_t shrt = t1 - t0, lng = t2 - t1;
    if (lng <= shrt) return 0;
    uint64_t hz = (lng - shrt) * 1000ull / 300ull;
    if (hz < 50000000ull || hz > 20000000000ull) return 0;
    return hz;
}

/* ------------------------------------------------------------- collision -- */

static int box_hits(float x, float y, float z) {
    int x0 = ifloor(x - HALFW), x1 = ifloor(x + HALFW);
    int z0 = ifloor(z - HALFW), z1 = ifloor(z + HALFW);
    int y0 = ifloor(y),         y1 = ifloor(y + BODY);
    for (int yy = y0; yy <= y1; yy++)
        for (int zz = z0; zz <= z1; zz++)
            for (int xx = x0; xx <= x1; xx++)
                if (blk_solid[blk_at(xx, yy, zz)]) return 1;
    return 0;
}

static int in_water(void) {
    return blk_at(ifloor(pfx), ifloor(pfy + 0.6f), ifloor(pfz)) == B_WATER;
}

/* ------------------------------------------------------------------ hud --- */

static void say(const char *s) {
    snprintf(msg, sizeof msg, "%s", s);
    msg_left = 2.6f;
}

static void draw_crosshair(void) {
    int cx = (int)FB.w / 2, cy = (int)FB.h / 2;
    hud_rect(fbbuf, FB.w, FB.h, cx - 11, cy - 1, 22, 3, 0x00101010u);
    hud_rect(fbbuf, FB.w, FB.h, cx - 1, cy - 11, 3, 22, 0x00101010u);
    hud_rect(fbbuf, FB.w, FB.h, cx - 10, cy, 20, 1, 0x00E8E8E8u);
    hud_rect(fbbuf, FB.w, FB.h, cx, cy - 10, 1, 20, 0x00E8E8E8u);
}

static void draw_hotbar(void) {
    const int tile = 44, gap = 4, n = 10;
    int w = n * tile + (n - 1) * gap;
    int x0 = ((int)FB.w - w) / 2;
    int y0 = (int)FB.h - tile - 26;

    hud_rect(fbbuf, FB.w, FB.h, x0 - 6, y0 - 6, w + 12, tile + 12, 0x00101418u);
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (tile + gap);
        hud_rect(fbbuf, FB.w, FB.h, x, y0, tile, tile, 0x00202830u);
        hud_block_icon(fbbuf, FB.w, FB.h, x + 3, y0 + 3, tile - 6, bar[i]);

        uint32_t border = (i == slot) ? 0x00FFFFFFu : 0x00404A54u;
        int t = (i == slot) ? 3 : 1;
        hud_rect(fbbuf, FB.w, FB.h, x - t, y0 - t, tile + 2 * t, t, border);
        hud_rect(fbbuf, FB.w, FB.h, x - t, y0 + tile, tile + 2 * t, t, border);
        hud_rect(fbbuf, FB.w, FB.h, x - t, y0, t, tile, border);
        hud_rect(fbbuf, FB.w, FB.h, x + tile, y0, t, tile, border);

        char k[2] = { (char)(i == 9 ? '0' : '1' + i), 0 };
        hud_text_shadow(fbbuf, FB.w, FB.h, x + 2, y0 + tile - 10, k, 0x00C8D0D8u, 1);
    }

    char line[64];
    snprintf(line, sizeof line, "%s", blk_name[bar[slot]]);
    int tw = (int)strlen(line) * 16;
    hud_text_shadow(fbbuf, FB.w, FB.h, ((int)FB.w - tw) / 2, y0 - 32, line,
                    0x00FFFFFFu, 2);
}

static const char *HELP[] = {
    "wasd step   arrows look   space jump   z down",
    "e mine   q place   1-0 pick   r autorun   f fly",
    "[ ] view   - = detail   t time   y clock   h help",
    "k save   l load   g new world   esc quit",
};

static void draw_hud(float fps, const struct hit *sel) {
    char line[96];
    int y = 12;
    const uint32_t col = 0x00E6ECF2u;

    snprintf(line, sizeof line, "xyz %.1f %.1f %.1f", (double)pfx, (double)pfy, (double)pfz);
    hud_text_shadow(fbbuf, FB.w, FB.h, 12, y, line, col, 2); y += 20;

    int hh = (int)(phase * (12.0f / 3.14159265f)) + 6;
    snprintf(line, sizeof line, "%d:%02d   fps %.1f   detail 1/%d%s   view %d",
             ((hh % 24) + 24) % 24,
             (int)((phase * (720.0f / 3.14159265f))) % 60,
             (double)fps, render_get_scale(),
             auto_detail == 1 ? " auto" : (auto_detail == 2 ? " fixed" : ""),
             render_get_dist());
    hud_text_shadow(fbbuf, FB.w, FB.h, 12, y, line, col, 2); y += 20;

    if (sel->ok)
        snprintf(line, sizeof line, "looking at %s  %d %d %d",
                 blk_name[blk_at(sel->bx, sel->by, sel->bz)], sel->bx, sel->by, sel->bz);
    else
        snprintf(line, sizeof line, "looking at nothing");
    hud_text_shadow(fbbuf, FB.w, FB.h, 12, y, line, col, 2); y += 20;

    if (fly || autorun) {
        snprintf(line, sizeof line, "%s%s", fly ? "[fly] " : "", autorun ? "[autorun]" : "");
        hud_text_shadow(fbbuf, FB.w, FB.h, 12, y, line, 0x00FFD880u, 2); y += 20;
    }

    if (msg_left > 0.0f)
        hud_text_shadow(fbbuf, FB.w, FB.h, 12, y, msg, 0x0090FF90u, 2);

    if (show_hud) {
        int n = (int)(sizeof HELP / sizeof HELP[0]);
        int hy = (int)FB.h - 26 - 44 - 34 - n * 16;
        for (int i = 0; i < n; i++)
            hud_text_shadow(fbbuf, FB.w, FB.h, 12, hy + i * 16, HELP[i], 0x00A8B4C0u, 1);
    }
}

static void splash(const char *what) {
    for (unsigned y = 0; y < FB.h; y++) {
        uint32_t c = mix32(0x0E1A2Au, 0x1E3350u, (int)(y * 256 / FB.h));
        for (unsigned x = 0; x < FB.w; x++) fbbuf[(size_t)y * FB.w + x] = c;
    }
    int w = (int)strlen(what) * 24;
    hud_text_shadow(fbbuf, FB.w, FB.h, ((int)FB.w - w) / 2, (int)FB.h / 2 - 12,
                    what, 0x00FFFFFFu, 3);
    fb_present(fbbuf);
}

/* ------------------------------------------------------------------ main -- */

static void respawn(void) {
    world_spawn(&pfx, &pfy, &pfz);
    vx = vy = vz = 0.0f;
    tvel = pvel = 0.0f;
    autorun = 0;
}

int main(int argc, char **argv) {
    if (bolt_abi() != BOLT_ABI_VERSION) {
        printf("boltcraft: built for BoltOS ABI %d, this kernel speaks %d\n",
               BOLT_ABI_VERSION, bolt_abi());
        return 1;
    }

    uint32_t seed = 20260916u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-seed") && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            printf("usage: boltcraft [-seed N]\n");
            return 2;
        }
    }

    uint64_t hz = tsc_hz();
    if (!hz) hz = tsc_hz_by_accept();
    if (hz) printf("boltcraft: cpu counter runs at %u kHz\n", (unsigned)(hz / 1000ull));
    else    printf("boltcraft: no usable clock; fixed timestep, detail stays put\n");

    /* Take the screen before doing four seconds of terrain: without a panel
     * this cannot run at all, and generating a world first only means finding
     * that out later. It also means the first world gets the same "generating"
     * card that G gets, instead of a black screen. */
    if (fb_map(&FB) != 0) {
        printf("boltcraft: no framebuffer -- is something else holding it?\n");
        return 1;
    }

    size_t px = (size_t)FB.w * FB.h;
    fbbuf = (uint32_t *)malloc(px * 4);
    rbuf  = (uint32_t *)malloc(px * 4);
    if (!fbbuf || !rbuf) {
        fb_release();
        printf("boltcraft: no room for a %ux%u frame\n", FB.w, FB.h);
        return 1;
    }

    render_init(rbuf, (int)FB.w, (int)FB.h);
    /* 1/3 on a 1024x768 panel is 342x256 rays. Comfortable on hardware,
     * survivable under emulation; '-' and '=' move it if it is not, and after
     * a few frames the chooser will have moved it anyway. */
    render_set_scale(3);
    splash("generating");

    tex_build();

    world_alloc();
    if (!g_world) {
        fb_release();
        printf("boltcraft: 4 MiB for the world would not fit\n");
        return 1;
    }
    printf("boltcraft: generating world %u (256x256x64)\n", seed);
    uint64_t gt = rdtsc();
    world_generate(seed);
    respawn();
    if (hz) printf("boltcraft: terrain took %u ms\n",
                   (unsigned)((rdtsc() - gt) * 1000ull / hz));
    printf("boltcraft: spawn at %d %d %d\n", (int)pfx, (int)pfy, (int)pfz);

    uint64_t last = rdtsc();
    float fps = 0.0f, fps_acc = 0.0f, was_fps = 0.0f;
    int   fps_n = 0, was_scale = 0;
    uint64_t frames = 0;

    say("welcome -- h toggles the key list");

    int running = 1;
    while (running) {
        uint64_t now = rdtsc();
        /* Two different numbers on purpose. `raw` is how long the frame really
         * took and is what the fps readout and the detail chooser want; `dt` is
         * clamped, because one 800 ms hitch applied to the physics teleports
         * you through a wall. Feeding the clamped one to the fps counter --
         * which is what the first version did -- made a 1 fps machine report a
         * confident 6 and never step the detail down. */
        float raw = hz ? (float)(now - last) / (float)hz : 1.0f / 30.0f;
        float dt = fclampf(raw, 1.0f / 240.0f, 1.0f / 5.0f);
        last = now;
        frames++;

        fps_acc += raw; fps_n++;
        if (fps_acc > 0.7f) {
            fps = (float)fps_n / fps_acc; fps_acc = 0.0f; fps_n = 0;

            /* Pick the detail setting by measurement, not by guessing what the
             * machine is: the same binary runs on a laptop panel and inside
             * QEMU's interpreter, and those are two orders of magnitude apart.
             *
             * The catch is that below some resolution the rays stop being the
             * expensive part. SYS_FBPRESENT copies the whole panel however
             * little of it you actually computed, so on a machine with no
             * virtualisation there is a floor of about 70 ms that no detail
             * setting can get under -- and a chooser that only watches the
             * frame rate walks all the way down to 1/8 buying nothing. So a
             * step that does not pay for itself is taken back, and that is
             * where it stays. */
            if (auto_detail == 1 && hz && frames > 3) {
                int s = render_get_scale();
                if (was_scale && s > was_scale && fps < was_fps * 1.12f) {
                    render_set_scale(was_scale);
                    auto_detail = 2;
                } else {
                    was_scale = s; was_fps = fps;
                    if (fps < 15.0f && s < 8)      render_set_scale(s + 1);
                    else if (fps > 42.0f && s > 1) render_set_scale(s - 1);
                }
            }
        }

        cam.x = pfx; cam.y = pfy + EYE; cam.z = pfz;
        struct hit sel;
        ray_pick(&cam, 5.6f, &sel);

        /* ---- input ------------------------------------------------------ */
        float fwd = 0.0f, side = 0.0f, lift = 0.0f;
        int jump = 0;
        for (int k; (k = getkey()) >= 0; ) {
            switch (k) {
            case 'w': case 'W': fwd += 1.0f; break;
            case 's': case 'S': fwd -= 1.0f; autorun = 0; break;
            case 'a': case 'A': side -= 1.0f; break;
            case 'd': case 'D': side += 1.0f; break;
            case K_LEFT:  tvel -= 2.7f; break;
            case K_RIGHT: tvel += 2.7f; break;
            case K_UP:    pvel += 2.0f; break;
            case K_DOWN:  pvel -= 2.0f; break;
            case ' ':     jump = 1; lift += 1.0f; break;
            case 'z': case 'Z': lift -= 1.0f; break;
            case 'f': case 'F':
                fly = !fly; vy = 0.0f;
                say(fly ? "fly on" : "fly off");
                break;
            case 'r': case 'R':
                autorun = !autorun;
                say(autorun ? "autorun on" : "autorun off");
                break;
            case 'e': case 'E':
                if (sel.ok) {
                    uint8_t b = blk_at(sel.bx, sel.by, sel.bz);
                    if (b == B_BEDROCK) say("bedrock does not break");
                    else world_set(sel.bx, sel.by, sel.bz, B_AIR);
                }
                break;
            case 'q': case 'Q':
                if (sel.ok) {
                    int nx = sel.bx + sel.nx, ny = sel.by + sel.ny, nz = sel.bz + sel.nz;
                    uint8_t at = blk_at(nx, ny, nz);
                    if ((unsigned)ny < WY && (at == B_AIR || at == B_WATER)) {
                        uint8_t was = at;
                        world_set(nx, ny, nz, bar[slot]);
                        if (box_hits(pfx, pfy, pfz)) {        /* not inside your own head */
                            world_set(nx, ny, nz, was);
                            say("no room for that");
                        }
                    }
                }
                break;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                slot = k - '1'; break;
            case '0': slot = 9; break;
            case '[': render_set_dist(render_get_dist() - 8); break;
            case ']': render_set_dist(render_get_dist() + 8); break;
            case '-': case '_':
                render_set_scale(render_get_scale() + 1);
                if (auto_detail) { auto_detail = 0; say("detail locked"); }
                break;
            case '=': case '+':
                render_set_scale(render_get_scale() - 1);
                if (auto_detail) { auto_detail = 0; say("detail locked"); }
                break;
            case 't': case 'T': phase += 0.22f; break;
            case 'y': case 'Y':
                clock_running = !clock_running;
                say(clock_running ? "clock running" : "clock stopped");
                break;
            case 'h': case 'H': show_hud = !show_hud; break;
            case 'k': case 'K':
                say(world_save(SAVE_PATH, &cam) == 0 ? "world saved" : "save failed");
                break;
            case 'l': case 'L':
                if (world_load(SAVE_PATH, &cam) == 0) {
                    pfx = cam.x; pfy = cam.y - EYE; pfz = cam.z;
                    vx = vy = vz = 0.0f;
                    say("world loaded");
                } else say("no saved world");
                break;
            case 'g': case 'G':
                splash("generating");
                seed = seed * 1664525u + 1013904223u;
                uint64_t g0 = rdtsc();
                world_generate(seed);
                respawn();
                if (hz) printf("boltcraft: terrain %u took %u ms\n", seed,
                               (unsigned)((rdtsc() - g0) * 1000ull / hz));
                say("new world");
                last = rdtsc();
                break;
            case K_ESC: running = 0; break;
            default: break;
            }
        }
        if (autorun) fwd += 1.0f;

        /* ---- look ------------------------------------------------------- */
        cam.yaw   += tvel * dt;
        cam.pitch += pvel * dt;
        cam.pitch = fclampf(cam.pitch, -1.5f, 1.5f);
        float ldecay = 1.0f - fclampf(dt / 0.15f, 0.0f, 1.0f);
        tvel *= ldecay; pvel *= ldecay;
        if (cam.yaw >  6.2831853f) cam.yaw -= 6.2831853f;
        if (cam.yaw < -6.2831853f) cam.yaw += 6.2831853f;

        /* ---- move ------------------------------------------------------- */
        float cy = cosf(cam.yaw), sy = sinf(cam.yaw);
        int wet = in_water();
        float push = fly ? 13.0f : (wet ? 4.4f : 7.4f);

        if (fwd != 0.0f || side != 0.0f) {
            float wx = sy * fwd + cy * side;
            float wz = cy * fwd - sy * side;
            float l = fsqrt(wx * wx + wz * wz);
            if (l > 0.0f) { wx /= l; wz /= l; }
            vx += wx * push;
            vz += wz * push;
            float sp = fsqrt(vx * vx + vz * vz);
            float cap = fly ? 17.0f : (wet ? 4.0f : 6.1f);
            if (sp > cap) { vx = vx / sp * cap; vz = vz / sp * cap; }
        }

        if (fly) {
            vy += lift * 13.0f;
            vy = fclampf(vy, -15.0f, 15.0f);
        } else if (wet) {
            vy += -9.0f * dt;
            if (jump) vy = 4.3f;
            if (vy < -3.2f) vy = -3.2f;
        } else {
            vy += -26.0f * dt;
            if (jump && onground) { vy = 8.2f; onground = 0; }
            if (vy < -55.0f) vy = -55.0f;
        }

        float nx = pfx + vx * dt;
        if (!box_hits(nx, pfy, pfz)) pfx = nx; else vx = 0.0f;
        float nz = pfz + vz * dt;
        if (!box_hits(pfx, pfy, nz)) pfz = nz; else vz = 0.0f;
        float ny = pfy + vy * dt;
        if (!box_hits(pfx, ny, pfz)) pfy = ny;
        else { if (vy < 0.0f) onground = 1; vy = 0.0f; }

        if (!fly) onground = box_hits(pfx, pfy - 0.06f, pfz);
        else onground = 0;

        /* Friction. Horizontal damping is a fixed half-life rather than a force
         * because the input is impulses -- one tap should be one step and then
         * a clean stop, not a slide. */
        float mdecay = 1.0f - fclampf(dt / (fly ? 0.30f : 0.20f), 0.0f, 1.0f);
        vx *= mdecay; vz *= mdecay;
        if (fly) vy *= mdecay;

        if (pfx < 0.0f) pfx += (float)WX;  else if (pfx >= (float)WX) pfx -= (float)WX;
        if (pfz < 0.0f) pfz += (float)WZ;  else if (pfz >= (float)WZ) pfz -= (float)WZ;
        if (pfy < -30.0f) respawn();                    /* should not happen; does */
        if (pfy > (float)WY - BODY - 0.1f) { pfy = (float)WY - BODY - 0.1f; if (vy > 0) vy = 0; }

        /* ---- draw ------------------------------------------------------- */
        if (clock_running) phase += dt * (6.2831853f / 540.0f);
        if (phase > 6.2831853f) phase -= 6.2831853f;
        float daylight = fclampf(sinf(phase) * 1.8f + 0.30f, 0.05f, 1.0f);

        cam.x = pfx; cam.y = pfy + EYE; cam.z = pfz;
        render_world(&cam, daylight, phase, &sel);
        render_blit(fbbuf, (int)FB.w, (int)FB.h);

        /* Underwater: a blue wash over everything, including the HUD, which is
         * cheating but reads correctly and costs one pass. */
        if (blk_at(ifloor(cam.x), ifloor(cam.y), ifloor(cam.z)) == B_WATER) {
            uint32_t tint = scale32(0x1A5A96u, 40 + (int)(daylight * 150.0f));
            for (size_t i = 0; i < px; i++) fbbuf[i] = mix32(fbbuf[i], tint, 96);
        }

        draw_crosshair();
        draw_hud(fps, &sel);
        draw_hotbar();
        if (msg_left > 0.0f) msg_left -= dt;

        fb_present(fbbuf);
        yield();
    }

    fb_release();
    printf("boltcraft: %u frames, screen handed back\n", (unsigned)frames);
    printf("boltcraft: left standing at %d %d %d\n", (int)pfx, (int)pfy, (int)pfz);
    free(fbbuf); free(rbuf); free(g_world);
    return 0;
}
