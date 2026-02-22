/*
 * perf_bench.c — GPU Performance Benchmark
 *
 * Renders increasing numbers of primitives and displays:
 *   - Current primitive count
 *   - FPS (from VBlank counter)
 *   - GPU status (busy/ready)
 *
 * Phases (auto-cycle every 5 seconds):
 *   1. Flat-shaded triangles (increasing count)
 *   2. Flat-shaded quads
 *   3. Large fill rects (fill rate)
 *   4. Mixed primitives
 *
 * R3000-Emu DevPSX test suite — 2026
 */
#include <sys/types.h>
#include <stdio.h>
#include <libgte.h>
#include <libetc.h>
#include <libgpu.h>

#define SCREENXRES 320
#define SCREENYRES 240
#define OTLEN      16
#define PHASE_FRAMES (60 * 5) /* 5 seconds per phase at 60fps */

DISPENV disp[2];
DRAWENV draw[2];
u_long  ot[2][OTLEN];
char    primbuff[2][65536]; /* Large buffer for many primitives */
char   *nextpri = primbuff[0];
short   db = 0;

/* Simple LCG pseudo-random */
static unsigned int rand_seed = 12345;
static int psx_rand(void)
{
    rand_seed = rand_seed * 1103515245 + 12345;
    return (int)((rand_seed >> 16) & 0x7FFF);
}

static void init(void)
{
    ResetGraph(0);
    InitGeom();
    SetGeomOffset(SCREENXRES / 2, SCREENYRES / 2);
    SetGeomScreen(SCREENXRES / 2);

    SetDefDispEnv(&disp[0], 0, 0, SCREENXRES, SCREENYRES);
    SetDefDispEnv(&disp[1], 0, SCREENYRES, SCREENXRES, SCREENYRES);
    SetDefDrawEnv(&draw[0], 0, SCREENYRES, SCREENXRES, SCREENYRES);
    SetDefDrawEnv(&draw[1], 0, 0, SCREENXRES, SCREENYRES);

    SetDispMask(1);

    setRGB0(&draw[0], 0, 0, 32);
    setRGB0(&draw[1], 0, 0, 32);
    draw[0].isbg = 1;
    draw[1].isbg = 1;

    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);

    FntLoad(960, 0);
    FntOpen(4, 4, 312, 64, 0, 512);
}

static void display(void)
{
    DrawSync(0);
    VSync(0);
    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);
    DrawOTag(&ot[db][OTLEN - 1]);
    db = !db;
    nextpri = primbuff[db];
}

static void draw_flat_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                           unsigned char r, unsigned char g, unsigned char b)
{
    POLY_F3 *p = (POLY_F3 *)nextpri;
    setPolyF3(p);
    setRGB0(p, r, g, b);
    setXY3(p, x0, y0, x1, y1, x2, y2);
    addPrim(ot[db], p);
    nextpri += sizeof(POLY_F3);
}

static void draw_flat_quad(int x, int y, int w, int h,
                            unsigned char r, unsigned char g, unsigned char b)
{
    POLY_F4 *p = (POLY_F4 *)nextpri;
    setPolyF4(p);
    setRGB0(p, r, g, b);
    setXY4(p, x, y, x + w, y, x, y + h, x + w, y + h);
    addPrim(ot[db], p);
    nextpri += sizeof(POLY_F4);
}

static void draw_fill_rect(int x, int y, int w, int h,
                            unsigned char r, unsigned char g, unsigned char b)
{
    TILE *t = (TILE *)nextpri;
    setTile(t);
    setRGB0(t, r, g, b);
    setXY0(t, x, y);
    setWH(t, w, h);
    addPrim(ot[db], t);
    nextpri += sizeof(TILE);
}

int main(void)
{
    int frame = 0;
    int phase = 0;
    int prim_count = 10; /* Starting primitive count */
    int fps_counter = 0;
    int fps_display = 0;
    int fps_timer = 0;
    int i;
    const char *phase_names[] = {
        "FLAT TRIS", "FLAT QUADS", "FILL RECTS", "MIXED"
    };

    init();

    while (1)
    {
        ClearOTagR(ot[db], OTLEN);

        /* Draw primitives based on current phase */
        for (i = 0; i < prim_count; i++)
        {
            int rx = psx_rand() % (SCREENXRES - 40);
            int ry = 30 + (psx_rand() % (SCREENYRES - 70));
            unsigned char cr = (unsigned char)(psx_rand() & 0xFF);
            unsigned char cg = (unsigned char)(psx_rand() & 0xFF);
            unsigned char cb = (unsigned char)(psx_rand() & 0xFF);

            switch (phase)
            {
            case 0: /* Flat triangles */
                draw_flat_tri(rx, ry, rx + 20 + (psx_rand() % 20), ry + 10,
                              rx + 10, ry + 15 + (psx_rand() % 15),
                              cr, cg, cb);
                break;

            case 1: /* Flat quads */
                draw_flat_quad(rx, ry, 15 + (psx_rand() % 25), 15 + (psx_rand() % 25),
                               cr, cg, cb);
                break;

            case 2: /* Large fill rects */
                draw_fill_rect(rx, ry, 40 + (psx_rand() % 60), 30 + (psx_rand() % 50),
                               cr, cg, cb);
                break;

            case 3: /* Mixed */
                if (i % 3 == 0)
                    draw_flat_tri(rx, ry, rx + 25, ry + 12, rx + 12, ry + 20, cr, cg, cb);
                else if (i % 3 == 1)
                    draw_flat_quad(rx, ry, 20, 20, cr, cg, cb);
                else
                    draw_fill_rect(rx, ry, 50, 40, cr, cg, cb);
                break;
            }
        }

        /* FPS calculation: count frames over 60 VBlanks (~1 second) */
        fps_counter++;
        fps_timer++;
        if (fps_timer >= 60)
        {
            fps_display = fps_counter;
            fps_counter = 0;
            fps_timer = 0;
        }

        /* Increase primitive count every 60 frames */
        if ((frame % 60) == 0 && frame > 0)
        {
            if (prim_count < 2000)
                prim_count += 20;
        }

        /* Switch phase every PHASE_FRAMES */
        if ((frame % PHASE_FRAMES) == 0 && frame > 0)
        {
            phase = (phase + 1) % 4;
            prim_count = 10; /* Reset count for new phase */
        }

        /* Display stats */
        {
            /* Read GPUSTAT register directly (0x1F801814) */
            volatile unsigned int *gpustat = (volatile unsigned int *)0x1F801814;
            unsigned int stat = *gpustat;
            int gpu_ready = (stat >> 26) & 1;  /* bit 26 = ready to receive cmds */

            FntPrint("GPU PERF BENCH  phase=%d/%s\n", phase, phase_names[phase]);
            FntPrint("prims=%d  fps=%d  frame=%d\n", prim_count, fps_display, frame);
            FntPrint("GPUSTAT=%08x rdy=%d", stat, gpu_ready);
        }
        FntFlush(-1);

        display();
        frame++;
    }

    return 0;
}
