/*
 * physics_ball.c — Bouncing Ball Physics / Timing Validation
 *
 * A ball bounces inside a box with gravity. The physics is stepped
 * once per VBlank (DrawSync + VSync), so timing accuracy directly
 * affects the bounce speed.
 *
 * On-screen display:
 *   - Ball position and velocity
 *   - Frame count, VBlanks/sec
 *   - Timing health indicator (GREEN / YELLOW / RED)
 *
 * Reference: at exactly 60 VBlanks/sec, the ball should hit the
 * floor (~Y=210) at approximately frame 14 starting from Y=20
 * with gravity = 2 px/frame^2.
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

/* Physics constants (fixed-point 8.8) */
#define FP_SHIFT    8
#define FP_ONE      (1 << FP_SHIFT)
#define GRAVITY     (2 * FP_ONE)        /* 2 px/frame^2 in 8.8 */
#define RESTITUTION 230                 /* 0.90 * 256 = 230 */
#define BALL_SIZE   12

/* Bounding box (pixel coords) */
#define BOX_LEFT    20
#define BOX_RIGHT   300
#define BOX_TOP     50
#define BOX_BOTTOM  220

DISPENV disp[2];
DRAWENV draw[2];
u_long  ot[2][OTLEN];
char    primbuff[2][8192];
char   *nextpri = primbuff[0];
short   db = 0;

/* Ball state (fixed-point 8.8) */
static int ball_x, ball_y;
static int ball_vx, ball_vy;

/* Timing */
static int frame_count;
static int fps_counter;
static int fps_display;
static int fps_timer;
static int first_floor_hit_frame;

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

    setRGB0(&draw[0], 20, 20, 40);
    setRGB0(&draw[1], 20, 20, 40);
    draw[0].isbg = 1;
    draw[1].isbg = 1;

    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);

    FntLoad(960, 0);
    FntOpen(4, 4, 312, 48, 0, 512);

    /* Initial ball state: top-center, no velocity */
    ball_x  = ((BOX_LEFT + BOX_RIGHT) / 2) << FP_SHIFT;
    ball_y  = (BOX_TOP + 10) << FP_SHIFT;
    ball_vx = (1 * FP_ONE) + (FP_ONE / 2); /* 1.5 px/frame horizontal */
    ball_vy = 0;

    frame_count = 0;
    fps_counter = 0;
    fps_display = 0;
    fps_timer = 0;
    first_floor_hit_frame = -1;
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

static void physics_step(void)
{
    int px, py;

    /* Apply gravity */
    ball_vy += GRAVITY;

    /* Integrate position */
    ball_x += ball_vx;
    ball_y += ball_vy;

    /* Pixel position for collision */
    px = ball_x >> FP_SHIFT;
    py = ball_y >> FP_SHIFT;

    /* Floor bounce */
    if (py + BALL_SIZE >= BOX_BOTTOM)
    {
        ball_y = (BOX_BOTTOM - BALL_SIZE) << FP_SHIFT;
        ball_vy = -(ball_vy * RESTITUTION / 256);

        if (first_floor_hit_frame < 0)
            first_floor_hit_frame = frame_count;
    }

    /* Ceiling bounce */
    if (py <= BOX_TOP)
    {
        ball_y = BOX_TOP << FP_SHIFT;
        ball_vy = -(ball_vy * RESTITUTION / 256);
    }

    /* Wall bounces */
    if (px <= BOX_LEFT)
    {
        ball_x = BOX_LEFT << FP_SHIFT;
        ball_vx = -(ball_vx * RESTITUTION / 256);
        if (ball_vx == 0) ball_vx = FP_ONE; /* Prevent sticking */
    }
    if (px + BALL_SIZE >= BOX_RIGHT)
    {
        ball_x = (BOX_RIGHT - BALL_SIZE) << FP_SHIFT;
        ball_vx = -(ball_vx * RESTITUTION / 256);
        if (ball_vx == 0) ball_vx = -FP_ONE;
    }
}

int main(void)
{
    int px, py;
    POLY_F4 *box_border;
    POLY_F4 *box_inner;
    POLY_F4 *ball;

    init();

    while (1)
    {
        ClearOTagR(ot[db], OTLEN);

        /* Step physics */
        physics_step();

        px = ball_x >> FP_SHIFT;
        py = ball_y >> FP_SHIFT;

        /* Draw bounding box (border) */
        box_border = (POLY_F4 *)nextpri;
        setPolyF4(box_border);
        setRGB0(box_border, 128, 128, 128);
        setXY4(box_border,
               BOX_LEFT - 2, BOX_TOP - 2,
               BOX_RIGHT + 2, BOX_TOP - 2,
               BOX_LEFT - 2, BOX_BOTTOM + 2,
               BOX_RIGHT + 2, BOX_BOTTOM + 2);
        addPrim(&ot[db][2], box_border);  /* depth 2 = behind */
        nextpri += sizeof(POLY_F4);

        /* Draw bounding box (inner = background color) */
        box_inner = (POLY_F4 *)nextpri;
        setPolyF4(box_inner);
        setRGB0(box_inner, 20, 20, 40);
        setXY4(box_inner,
               BOX_LEFT, BOX_TOP,
               BOX_RIGHT, BOX_TOP,
               BOX_LEFT, BOX_BOTTOM,
               BOX_RIGHT, BOX_BOTTOM);
        addPrim(&ot[db][1], box_inner);   /* depth 1 = middle */
        nextpri += sizeof(POLY_F4);

        /* Draw ball */
        ball = (POLY_F4 *)nextpri;
        setPolyF4(ball);

        /* Color-coded: GREEN = good timing, YELLOW = close, RED = off */
        if (fps_display >= 59 && fps_display <= 61)
            setRGB0(ball, 0, 255, 0);     /* GREEN — perfect 60Hz */
        else if (fps_display >= 55 && fps_display <= 65)
            setRGB0(ball, 255, 255, 0);   /* YELLOW — close */
        else if (fps_display > 0)
            setRGB0(ball, 255, 0, 0);     /* RED — timing is off */
        else
            setRGB0(ball, 255, 255, 255); /* WHITE — measuring... */

        setXY4(ball,
               px, py,
               px + BALL_SIZE, py,
               px, py + BALL_SIZE,
               px + BALL_SIZE, py + BALL_SIZE);
        addPrim(&ot[db][0], ball);        /* depth 0 = front (rendered last) */
        nextpri += sizeof(POLY_F4);

        /* FPS counter: count frames per 60 VBlanks */
        fps_counter++;
        fps_timer++;
        if (fps_timer >= 60)
        {
            fps_display = fps_counter;
            fps_counter = 0;
            fps_timer = 0;
        }

        /* Display stats */
        FntPrint("PHYSICS BALL  fps=%d  frame=%d\n", fps_display, frame_count);
        FntPrint("pos=(%d,%d) vel=(%d,%d)\n",
                 px, py, ball_vx, ball_vy);
        FntPrint("1st_floor_hit=%d (expect~14)",
                 first_floor_hit_frame);
        FntFlush(-1);

        display();
        frame_count++;
    }

    return 0;
}
