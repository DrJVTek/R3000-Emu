/*
 * blend_test.c — Semi-Transparency Mode Validation
 *
 * Displays 4 columns, each showing a different PS1 semi-transparency mode:
 *   Column 0: Mode 0 — B/2 + F/2  (50% alpha blend)
 *   Column 1: Mode 1 — B + F      (additive)
 *   Column 2: Mode 2 — B - F      (subtractive)
 *   Column 3: Mode 3 — B + F/4    (quarter additive)
 *
 * Each column has a bright background rect and a semi-transparent
 * foreground quad drawn on top. Compare visually with DuckStation.
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

DISPENV disp[2];
DRAWENV draw[2];
u_long  ot[2][OTLEN];
char    primbuff[2][8192];
char   *nextpri = primbuff[0];
short   db = 0;

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

    /* Dark grey background */
    setRGB0(&draw[0], 40, 40, 40);
    setRGB0(&draw[1], 40, 40, 40);
    draw[0].isbg = 1;
    draw[1].isbg = 1;

    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);

    FntLoad(960, 0);
    FntOpen(8, 8, 304, 48, 0, 256);
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

/* Draw a flat opaque quad */
static void draw_opaque_quad(int x, int y, int w, int h,
                             unsigned char r, unsigned char g, unsigned char b)
{
    POLY_F4 *p = (POLY_F4 *)nextpri;
    setPolyF4(p);
    setRGB0(p, r, g, b);
    setXY4(p, x, y, x + w, y, x, y + h, x + w, y + h);
    addPrim(&ot[db][2], p);   /* OT depth 2 = far = rendered first (behind) */
    nextpri += sizeof(POLY_F4);
}

/* Draw a flat semi-transparent quad with given blend mode.
 * We set the texpage E1h command to select the semi-transparency mode,
 * then draw a semi-transparent POLY_F4. */
static void draw_semi_quad(int x, int y, int w, int h,
                           unsigned char r, unsigned char g, unsigned char b,
                           int semi_mode)
{
    DR_TPAGE *tp;
    POLY_F4  *p;

    /* Semi-transparent flat quad — add FIRST so it's later in the LIFO chain */
    p = (POLY_F4 *)nextpri;
    setPolyF4(p);
    setSemiTrans(p, 1);
    setRGB0(p, r, g, b);
    setXY4(p, x, y, x + w, y, x, y + h, x + w, y + h);
    addPrim(&ot[db][0], p);
    nextpri += sizeof(POLY_F4);

    /* Set semi-transparency mode via texpage (bits 5-6)
     * Added AFTER the quad → LIFO puts it BEFORE the quad in the chain.
     * GPU sees: E1h(mode) then semi_quad — correct ordering. */
    tp = (DR_TPAGE *)nextpri;
    setDrawTPage(tp, 0, 0, getTPage(0, semi_mode, 0, 0));
    addPrim(&ot[db][0], tp);
    nextpri += sizeof(DR_TPAGE);
}

int main(void)
{
    int frame = 0;
    int col_w = 72;  /* column width */
    int gap   = 6;   /* gap between columns */
    int x0    = 10;  /* left margin */
    int y_bg  = 50;  /* background rect Y */
    int h_bg  = 160; /* background rect height */
    int y_fg  = 80;  /* foreground semi-trans rect Y */
    int h_fg  = 100; /* foreground semi-trans rect height */

    init();

    while (1)
    {
        int i;
        int x;

        ClearOTagR(ot[db], OTLEN);

        /* Draw 4 columns */
        for (i = 0; i < 4; i++)
        {
            x = x0 + i * (col_w + gap);

            /* Background: bright colored rect (opaque) */
            draw_opaque_quad(x, y_bg, col_w, h_bg,
                             (i == 0 || i == 3) ? 200 : 80,
                             (i == 1 || i == 3) ? 200 : 80,
                             (i == 2)           ? 200 : 80);

            /* Foreground: semi-transparent quad (white) with mode i */
            draw_semi_quad(x + 8, y_fg, col_w - 16, h_fg,
                           128, 128, 128, i);
        }

        /* Labels */
        FntPrint("SEMI-TRANS BLEND TEST  frame=%d\n", frame);
        FntPrint(" M0:B/2+F/2  M1:B+F  M2:B-F  M3:B+F/4");
        FntFlush(-1);

        display();
        frame++;
    }

    return 0;
}
