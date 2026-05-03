/*
 * doodle_jump.c — Vertical platformer demo, PSX / PsyQ 4.7 port.
 *
 * Rewritten on top of the PsyQ SDK (same pattern as physics_ball.c):
 *   - Double-buffered DISPENV/DRAWENV + OT (ordering table)
 *   - TILE primitives for every solid rectangle (1:1 with GP0 0x60h)
 *   - libetc PadInit/PadRead for controller input
 *   - VSync(0) for the frame cadence (locked 60 Hz NTSC / 50 Hz PAL)
 *   - FntPrint / FntFlush for on-screen HUD text
 *
 * Gameplay is unchanged from the hardware-direct draft:
 *   - Jump from platform to platform, camera follows upward progress.
 *   - Side walls wrap (exit left → reappear right).
 *   - Fall below the view → GAME OVER, press CROSS to retry.
 *
 * R3000-Emu DevPSX test suite — 2026
 */
#include <sys/types.h>
#include <stdio.h>
#include <libgte.h>   /* libgpu.h pulls SVECTOR from here */
#include <libetc.h>
#include <libgpu.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define OTLEN    8

#define PLATFORM_COUNT 14
#define PLAYER_W       12
#define PLAYER_H       16
#define PLATFORM_W     44
#define PLATFORM_H      8

/* PsyQ pad mask (active-LOW: bit cleared = button pressed). */
#define PAD_START 0x0008
#define PAD_LEFT  0x0080
#define PAD_RIGHT 0x0020
#define PAD_CROSS 0x4000

/* OT depth layers — higher index = drawn first = further back. */
#define Z_STARS      5
#define Z_GROUND     4
#define Z_PLATFORM   3
#define Z_PLAYER     2
#define Z_HUD        1
#define Z_GAME_OVER  0

DISPENV disp[2];
DRAWENV draw[2];
u_long  ot[2][OTLEN];
char    primbuff[2][8192];
char   *nextpri = primbuff[0];
short   db = 0;

struct Platform
{
    int x;
    int y;
};

static struct Platform platforms[PLATFORM_COUNT];

static int  player_x;
static int  player_y;
static int  vel_x;
static int  vel_y;
static int  camera_y;
static int  best_height;
static unsigned int rng_state;
static unsigned int frame_count;
static int  game_over;

static void init(void)
{
    ResetGraph(0);

    /* Double-buffered 320×240 in top/bottom halves of VRAM. */
    SetDefDispEnv(&disp[0], 0, 0,        SCREEN_W, SCREEN_H);
    SetDefDispEnv(&disp[1], 0, SCREEN_H, SCREEN_W, SCREEN_H);
    SetDefDrawEnv(&draw[0], 0, SCREEN_H, SCREEN_W, SCREEN_H);
    SetDefDrawEnv(&draw[1], 0, 0,        SCREEN_W, SCREEN_H);

    /* isbg=1 → GPU clears the draw area to this RGB each frame (fast fill). */
    setRGB0(&draw[0], 5, 8, 22);
    setRGB0(&draw[1], 5, 8, 22);
    draw[0].isbg = 1;
    draw[1].isbg = 1;

    SetDispMask(1);
    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);

    /* Debug font upload + window. */
    FntLoad(960, 0);
    FntOpen(8, 8, 260, 40, 0, 256);

    /* Standard digital pad, both slots. */
    PadInit(0);
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

static int rand_range(int max_value)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % (unsigned int)max_value);
}

static void reset_game(void)
{
    int i;

    rng_state   = 0x12345678u;
    player_x    = 154;
    player_y    = 188;
    vel_x       = 0;
    vel_y       = -72;
    camera_y    = 0;
    best_height = 0;
    frame_count = 0;
    game_over   = 0;

    for (i = 0; i < PLATFORM_COUNT; ++i)
    {
        platforms[i].x = 18 + rand_range(SCREEN_W - PLATFORM_W - 36);
        platforms[i].y = 220 - i * 34;
    }
    /* Guarantee a safe starting platform directly under the player. */
    platforms[0].x = 138;
    platforms[0].y = 212;
}

static void recycle_platforms(void)
{
    int i;
    int j;
    int min_y;
    int bottom_limit;

    /* Highest platform currently alive (smallest Y). */
    min_y = platforms[0].y;
    for (i = 1; i < PLATFORM_COUNT; ++i)
    {
        if (platforms[i].y < min_y)
            min_y = platforms[i].y;
    }

    /* Any platform that has fallen off-screen is respawned at the top. */
    bottom_limit = camera_y + SCREEN_H + 24;
    for (i = 0; i < PLATFORM_COUNT; ++i)
    {
        if (platforms[i].y > bottom_limit)
        {
            min_y -= 29 + rand_range(22);
            platforms[i].y = min_y;
            platforms[i].x = 8 + rand_range(SCREEN_W - PLATFORM_W - 16);

            /* Nudge away from neighbours so clusters don't form. */
            for (j = 0; j < PLATFORM_COUNT; ++j)
            {
                if (j != i &&
                    platforms[i].y > platforms[j].y - 12 &&
                    platforms[i].y < platforms[j].y + 12 &&
                    platforms[i].x > platforms[j].x - 48 &&
                    platforms[i].x < platforms[j].x + 48)
                {
                    platforms[i].x =
                        (platforms[i].x + 96) % (SCREEN_W - PLATFORM_W);
                }
            }
        }
    }
}

static void update_game(unsigned short pad)
{
    int i;
    int old_bottom;
    int new_bottom;

    /* START always resets; CROSS retries from GAME OVER. */
    if ((pad & PAD_START) == 0u)
        reset_game();

    if (game_over)
    {
        if ((pad & PAD_CROSS) == 0u)
            reset_game();
        return;
    }

    if ((pad & PAD_LEFT) == 0u)
        vel_x -= 5;
    if ((pad & PAD_RIGHT) == 0u)
        vel_x += 5;

    if (vel_x >  42) vel_x =  42;
    if (vel_x < -42) vel_x = -42;

    /* Integrate and apply friction + gravity. */
    old_bottom = player_y + PLAYER_H;
    player_x += vel_x >> 3;
    player_y += vel_y >> 3;
    new_bottom = player_y + PLAYER_H;

    vel_x = (vel_x * 7) >> 3; /* friction */
    vel_y += 5;               /* gravity */
    if (vel_y > 96)
        vel_y = 96;

    /* Horizontal wrap. */
    if (player_x < -PLAYER_W) player_x = SCREEN_W;
    if (player_x >  SCREEN_W) player_x = -PLAYER_W;

    /* Land on platforms only while falling. */
    if (vel_y > 0)
    {
        for (i = 0; i < PLATFORM_COUNT; ++i)
        {
            if (old_bottom <= platforms[i].y &&
                new_bottom >= platforms[i].y &&
                player_x + PLAYER_W > platforms[i].x &&
                player_x < platforms[i].x + PLATFORM_W)
            {
                vel_y   = -92;
                player_y = platforms[i].y - PLAYER_H;
                break;
            }
        }
    }

    /* Camera follows the player upward only. */
    if (player_y < camera_y + 92)
        camera_y = player_y - 92;

    if (-camera_y > best_height)
        best_height = -camera_y;

    recycle_platforms();

    if (player_y - camera_y > SCREEN_H + 32)
        game_over = 1;
}

/* Queue a flat-shaded rectangle at OT depth z. Clips to screen. */
static void add_tile(int x, int y, int w, int h,
                     int r, int g, int b, int z)
{
    TILE *t;

    if (w <= 0 || h <= 0)
        return;
    if (x >= SCREEN_W || y >= SCREEN_H || x + w <= 0 || y + h <= 0)
        return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;

    t = (TILE *)nextpri;
    setTile(t);
    setRGB0(t, (u_char)r, (u_char)g, (u_char)b);
    setXY0(t, (short)x, (short)y);
    setWH(t, (short)w, (short)h);
    addPrim(&ot[db][z], t);
    nextpri += sizeof(TILE);
}

static void draw_stars(void)
{
    int i;
    int x;
    int y;

    /* Parallax-scrolling dots against the blue-black sky. */
    for (i = 0; i < 18; ++i)
    {
        x = (i * 47 + (int)(frame_count >> 2)) % SCREEN_W;
        y = (i * 31 - (camera_y >> 4)) % SCREEN_H;
        if (y < 0)
            y += SCREEN_H;
        add_tile(x, y, 2, 2, 22, 34, 72, Z_STARS);
    }
}

static void draw_platforms(void)
{
    int i;
    int sx;
    int sy;

    for (i = 0; i < PLATFORM_COUNT; ++i)
    {
        sx = platforms[i].x;
        sy = platforms[i].y - camera_y;
        /* Body + lighter highlight strip. */
        add_tile(sx,     sy,     PLATFORM_W,     PLATFORM_H, 70, 220,  95, Z_PLATFORM);
        add_tile(sx + 2, sy + 1, PLATFORM_W - 4, 2,          150, 255, 155, Z_PLATFORM);
    }
}

static void draw_player(void)
{
    int sx = player_x;
    int sy = player_y - camera_y;

    if (game_over)
    {
        /* Red corpse + GAME OVER bar (text printed separately). */
        add_tile(sx, sy, PLAYER_W, PLAYER_H, 235, 68, 64, Z_PLAYER);
        add_tile(104, 106, 112, 12, 235, 68, 64, Z_GAME_OVER);
    }
    else
    {
        add_tile(sx, sy, PLAYER_W, PLAYER_H, 42, 220, 235, Z_PLAYER);
        /* Two white eye blocks. */
        add_tile(sx + 3, sy + 3, 3, 3, 255, 255, 255, Z_PLAYER);
        add_tile(sx + 8, sy + 3, 3, 3, 255, 255, 255, Z_PLAYER);
    }
}

static void draw_score_meter(void)
{
    int i;
    int bars = best_height / 80;
    if (bars > 18)
        bars = 18;

    for (i = 0; i < bars; ++i)
        add_tile(6 + i * 6, 6, 4, 8, 230, 210, 70, Z_HUD);
}

static void draw_ground(void)
{
    /* Always-visible red floor strip (painter's z = Z_GROUND). */
    add_tile(0, SCREEN_H - 5, SCREEN_W, 5, 210, 35, 42, Z_GROUND);
}

static void draw_scene(void)
{
    draw_stars();
    draw_ground();
    draw_platforms();
    draw_player();
    draw_score_meter();
}

int main(void)
{
    unsigned short pad;

    init();
    reset_game();

    while (1)
    {
        pad = (unsigned short)PadRead(0);
        update_game(pad);

        ClearOTagR(ot[db], OTLEN);
        draw_scene();

        FntPrint("DOODLE JUMP  best=%d  frame=%d\n",
                 best_height, frame_count);
        if (game_over)
            FntPrint("GAME OVER - press CROSS to retry");
        FntFlush(-1);

        display();
        ++frame_count;
    }

    return 0;
}
