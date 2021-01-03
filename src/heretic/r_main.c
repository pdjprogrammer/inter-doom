//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2016-2020 Julian Nechaevsky
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// R_main.c



#include <stdlib.h>
#include <math.h>
#include "doomdef.h"
#include "m_bbox.h"
#include "r_local.h"
#include "p_local.h"
#include "tables.h"
#include "i_timer.h"
#include "crispy.h"
#include "jn.h"

int viewangleoffset;

// haleyjd: removed WATCOMC

int validcount = 1;             // increment every time a check is made

lighttable_t *fixedcolormap;
extern lighttable_t **walllights;

int centerx, centery;
fixed_t centerxfrac, centeryfrac;
fixed_t projection;

int framecount;                 // just for profiling purposes

int sscount, linecount, loopcount;

fixed_t viewx, viewy, viewz;
angle_t viewangle;
fixed_t viewcos, viewsin;
player_t *viewplayer;

int detailshift;                // 0 = high, 1 = low

//
// precalculated math tables
//
angle_t clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup maps the visible view
// angles  to screen X coordinates, flattening the arc to a flat projection
// plane.  There will be many angles mapped to the same X.
int viewangletox[FINEANGLES / 2];

// The xtoviewangleangle[] table maps a screen pixel to the lowest viewangle
// that maps back to x ranges from clipangle to -clipangle
angle_t xtoviewangle[WIDESCREENWIDTH + 1];

lighttable_t *scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *scalelightfixed[MAXLIGHTSCALE];
lighttable_t *zlight[LIGHTLEVELS][MAXLIGHTZ];

// [JN] Floor brightmaps
lighttable_t* fullbright_blueonly_floor[LIGHTLEVELS][MAXLIGHTZ];

// [JN] Brightmaps
lighttable_t *fullbright_greenonly[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_redonly[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_blueonly[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_purpleonly[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_notbronze[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_flame[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_greenonly_dim[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_redonly_dim[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_blueonly_dim[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_yellowonly_dim[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t *fullbright_ethereal[LIGHTLEVELS][MAXLIGHTSCALE];

int extralight;                 // bumped light from gun blasts

void (*colfunc) (void);
void (*basecolfunc) (void);
void (*tlcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);

/*
===================
=
= R_AddPointToBox
=
===================
*/

void R_AddPointToBox(int x, int y, fixed_t * box)
{
    if (x < box[BOXLEFT])
        box[BOXLEFT] = x;
    if (x > box[BOXRIGHT])
        box[BOXRIGHT] = x;
    if (y < box[BOXBOTTOM])
        box[BOXBOTTOM] = y;
    if (y > box[BOXTOP])
        box[BOXTOP] = y;
}



/*
===============================================================================
=
= R_PointOnSide
=
= Returns side 0 (front) or 1 (back)
===============================================================================
*/

int R_PointOnSide(fixed_t x, fixed_t y, node_t * node)
{
    fixed_t dx, dy;
    fixed_t left, right;

    if (!node->dx)
    {
        if (x <= node->x)
            return node->dy > 0;
        return node->dy < 0;
    }
    if (!node->dy)
    {
        if (y <= node->y)
            return node->dx < 0;
        return node->dx > 0;
    }

    dx = (x - node->x);
    dy = (y - node->y);

// try to quickly decide by looking at sign bits
    if ((node->dy ^ node->dx ^ dx ^ dy) & 0x80000000)
    {
        if ((node->dy ^ dx) & 0x80000000)
            return 1;           // (left is negative)
        return 0;
    }

    left = FixedMul(node->dy >> FRACBITS, dx);
    right = FixedMul(dy, node->dx >> FRACBITS);

    if (right < left)
        return 0;               // front side
    return 1;                   // back side
}


int R_PointOnSegSide(fixed_t x, fixed_t y, seg_t * line)
{
    fixed_t lx, ly;
    fixed_t ldx, ldy;
    fixed_t dx, dy;
    fixed_t left, right;

    lx = line->v1->x;
    ly = line->v1->y;

    ldx = line->v2->x - lx;
    ldy = line->v2->y - ly;

    if (!ldx)
    {
        if (x <= lx)
            return ldy > 0;
        return ldy < 0;
    }
    if (!ldy)
    {
        if (y <= ly)
            return ldx < 0;
        return ldx > 0;
    }

    dx = (x - lx);
    dy = (y - ly);

// try to quickly decide by looking at sign bits
    if ((ldy ^ ldx ^ dx ^ dy) & 0x80000000)
    {
        if ((ldy ^ dx) & 0x80000000)
            return 1;           // (left is negative)
        return 0;
    }

    left = FixedMul(ldy >> FRACBITS, dx);
    right = FixedMul(dy, ldx >> FRACBITS);

    if (right < left)
        return 0;               // front side
    return 1;                   // back side
}


/*
===============================================================================
=
= R_PointToAngle
=
===============================================================================
*/

angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
    x -= viewx;
    y -= viewy;
    if ((!x) && (!y))
        return 0;
    if (x >= 0)
    {                           // x >=0
        if (y >= 0)
        {                       // y>= 0
            if (x > y)
                return tantoangle[SlopeDiv(y, x)];      // octant 0
            else
                return ANG90 - 1 - tantoangle[SlopeDiv(x, y)];  // octant 1
        }
        else
        {                       // y<0
            y = -y;
            if (x > y)
                return -tantoangle[SlopeDiv(y, x)];     // octant 8
            else
                return ANG270 + tantoangle[SlopeDiv(x, y)];     // octant 7
        }
    }
    else
    {                           // x<0
        x = -x;
        if (y >= 0)
        {                       // y>= 0
            if (x > y)
                return ANG180 - 1 - tantoangle[SlopeDiv(y, x)]; // octant 3
            else
                return ANG90 + tantoangle[SlopeDiv(x, y)];      // octant 2
        }
        else
        {                       // y<0
            y = -y;
            if (x > y)
                return ANG180 + tantoangle[SlopeDiv(y, x)];     // octant 4
            else
                return ANG270 - 1 - tantoangle[SlopeDiv(x, y)]; // octant 5
        }
    }

    return 0;
}


angle_t R_PointToAngle2(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    viewx = x1;
    viewy = y1;
    return R_PointToAngle(x2, y2);
}


fixed_t R_PointToDist(fixed_t x, fixed_t y)
{
    int angle;
    fixed_t dx, dy, temp;
    fixed_t dist;

    dx = abs(x - viewx);
    dy = abs(y - viewy);

    if (dy > dx)
    {
        temp = dx;
        dx = dy;
        dy = temp;
    }

    angle =
        (tantoangle[FixedDiv(dy, dx) >> DBITS] + ANG90) >> ANGLETOFINESHIFT;

    dist = FixedDiv(dx, finesine[angle]);       // use as cosine

    return dist;
}



/*
=================
=
= R_InitPointToAngle
=
=================
*/

void R_InitPointToAngle(void)
{
// now getting from tables.c
#if 0
    int i;
    long t;
    float f;
//
// slope (tangent) to angle lookup
//
    for (i = 0; i <= SLOPERANGE; i++)
    {
        f = atan((float) i / SLOPERANGE) / (3.141592657 * 2);
        t = 0xffffffff * f;
        tantoangle[i] = t;
    }
#endif
}

//=============================================================================

/*
================
=
= R_ScaleFromGlobalAngle
=
= Returns the texture mapping scale for the current line at the given angle
= rw_distance must be calculated first
================

// [crispy] WiggleFix: move R_ScaleFromGlobalAngle function to r_segs.c,
// above R_StoreWallRange

fixed_t R_ScaleFromGlobalAngle(angle_t visangle)
{
    fixed_t scale;
    int anglea, angleb;
    int sinea, sineb;
    fixed_t num, den;

#if 0
    {
        fixed_t dist, z;
        fixed_t sinv, cosv;

        sinv = finesine[(visangle - rw_normalangle) >> ANGLETOFINESHIFT];
        dist = FixedDiv(rw_distance, sinv);
        cosv = finecosine[(viewangle - visangle) >> ANGLETOFINESHIFT];
        z = abs(FixedMul(dist, cosv));
        scale = FixedDiv(projection, z);
        return scale;
    }
#endif

    anglea = ANG90 + (visangle - viewangle);
    angleb = ANG90 + (visangle - rw_normalangle);
// bothe sines are allways positive
    sinea = finesine[anglea >> ANGLETOFINESHIFT];
    sineb = finesine[angleb >> ANGLETOFINESHIFT];
    num = FixedMul(projection, sineb) << detailshift;
    den = FixedMul(rw_distance, sinea);
    if (den > num >> 16)
    {
        scale = FixedDiv(num, den);
        if (scale > 64 * FRACUNIT)
            scale = 64 * FRACUNIT;
        else if (scale < 256)
            scale = 256;
    }
    else
        scale = 64 * FRACUNIT;

    return scale;
}
*/


// [AM] Interpolate between two angles.
angle_t R_InterpolateAngle(angle_t oangle, angle_t nangle, fixed_t scale)
{
    if (nangle == oangle)
        return nangle;
    else if (nangle > oangle)
    {
        if (nangle - oangle < ANG270)
            return oangle + (angle_t)((nangle - oangle) * FIXED2DOUBLE(scale));
        else // Wrapped around
            return oangle - (angle_t)((oangle - nangle) * FIXED2DOUBLE(scale));
    }
    else // nangle < oangle
    {
        if (oangle - nangle < ANG270)
            return oangle - (angle_t)((oangle - nangle) * FIXED2DOUBLE(scale));
        else // Wrapped around
            return oangle + (angle_t)((nangle - oangle) * FIXED2DOUBLE(scale));
    }
}


/*
=================
=
= R_InitTables
=
=================
*/

void R_InitTables(void)
{
// now getting from tables.c
#if 0
    int i;
    float a, fv;
    int t;

//
// viewangle tangent table
//
    for (i = 0; i < FINEANGLES / 2; i++)
    {
        a = (i - FINEANGLES / 4 + 0.5) * PI * 2 / FINEANGLES;
        fv = FRACUNIT * tan(a);
        t = fv;
        finetangent[i] = t;
    }

//
// finesine table
//
    for (i = 0; i < 5 * FINEANGLES / 4; i++)
    {
// OPTIMIZE: mirror...
        a = (i + 0.5) * PI * 2 / FINEANGLES;
        t = FRACUNIT * sin(a);
        finesine[i] = t;
    }
#endif

}


/*
=================
=
= R_InitTextureMapping
=
=================
*/

void R_InitTextureMapping(void)
{
    int i;
    int x;
    int t;
    fixed_t focallength;


//
// use tangent table to generate viewangletox
// viewangletox will give the next greatest x after the view angle
//
    // calc focallength so FIELDOFVIEW angles covers SCREENWIDTH

    // [crispy] in widescreen mode, make sure the same number of horizontal
    // pixels shows the same part of the game scene as in regular rendering mode
    fixed_t focalwidth;
    focalwidth = (((ORIGWIDTH << hires)>>detailshift)/2)<<FRACBITS;

    focallength = FixedDiv(aspect_ratio >= 2 ? focalwidth : centerxfrac, 
                           finetangent[FINEANGLES / 4 + FIELDOFVIEW / 2]);

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        if (finetangent[i] > FRACUNIT * 2)
            t = -1;
        else if (finetangent[i] < -FRACUNIT * 2)
            t = viewwidth + 1;
        else
        {
            t = FixedMul(finetangent[i], focallength);
            t = (centerxfrac - t + FRACUNIT - 1) >> FRACBITS;
            if (t < -1)
                t = -1;
            else if (t > viewwidth + 1)
                t = viewwidth + 1;
        }
        viewangletox[i] = t;
    }

//
// scan viewangletox[] to generate xtoviewangleangle[]
//
// xtoviewangle will give the smallest view angle that maps to x
    for (x = 0; x <= viewwidth; x++)
    {
        i = 0;
        while (viewangletox[i] > x)
            i++;
        xtoviewangle[x] = (i << ANGLETOFINESHIFT) - ANG90;
    }

//
// take out the fencepost cases from viewangletox
//
    for (i = 0; i < FINEANGLES / 2; i++)
    {
        t = FixedMul(finetangent[i], focallength);
        t = centerx - t;
        if (viewangletox[i] == -1)
            viewangletox[i] = 0;
        else if (viewangletox[i] == viewwidth + 1)
            viewangletox[i] = viewwidth;
    }

    clipangle = xtoviewangle[0];
}

//=============================================================================

/*
====================
=
= R_InitLightTables
=
= Only inits the zlight table, because the scalelight table changes
= with view size
=
====================
*/

#define		DISTMAP	2

void R_InitLightTables(void)
{
    int i, j, level, startmap;
    int scale;

//
// Calculate the light levels to use for each level / distance combination
//
    for (i = 0; i < LIGHTLEVELS; i++)
    {
        startmap = ((LIGHTLEVELS - 1 - i) * 2) * NUMCOLORMAPS / LIGHTLEVELS;
        for (j = 0; j < MAXLIGHTZ; j++)
        {
            scale =
                FixedDiv((320 / 2 * FRACUNIT),
                         (j + 1) << LIGHTZSHIFT);
            scale >>= LIGHTSCALESHIFT;
            level = startmap - scale / DISTMAP;
            if (level < 0)
                level = 0;
            if (level >= NUMCOLORMAPS)
                level = NUMCOLORMAPS - 1;
            zlight[i][j] = colormaps + level * 256;
            
            // [JN] Floor brightmaps
            fullbright_blueonly_floor[i][j] = brightmaps_blueonly + level * 256;
        }
    }
}


/*
==============
=
= R_SetViewSize
=
= Don't really change anything here, because i might be in the middle of
= a refresh.  The change will take effect next refresh.
=
==============
*/

boolean setsizeneeded;
int setblocks, setdetail;

// [crispy] lookup table for horizontal screen coordinates
int		flipwidth[WIDESCREENWIDTH];

void R_SetViewSize(int blocks, int detail)
{
    setsizeneeded = true;
    setblocks = blocks;
    setdetail = detail;
}

/*
==============
=
= R_ExecuteSetViewSize
=
==============
*/

void R_ExecuteSetViewSize(void)
{
    fixed_t cosadj, dy;
    int i, j, level, startmap;

    setsizeneeded = false;

    // [JN] Set screen variables depending on aspect ratio.

    scaledviewwidth = screenwidth;

    if (aspect_ratio == 0 || aspect_ratio == 1)
    {
        // [JN] 4:3 and 5:4
        if (setblocks >= 11)
        {
            viewheight = SCREENHEIGHT;
        }
        else
        {
            scaledviewwidth = (setblocks * 32) << hires;
            viewheight = ((setblocks * 158 / 10)) << hires;
        }
    }
    else
    {
        // [JN] 16:9, 16:10 and 21:9
        if (setblocks == 9 || setblocks == 10)
        {
            viewheight = SCREENHEIGHT - SBARHEIGHT;
        }
        else
        {
            viewheight = SCREENHEIGHT;
        }
    }

    detailshift = setdetail;
    viewwidth = scaledviewwidth >> detailshift;

    centery = viewheight / 2;
    centerx = viewwidth / 2;
    centerxfrac = centerx << FRACBITS;
    centeryfrac = centery << FRACBITS;
    if (aspect_ratio >= 2)
    {
        projection = MIN(centerxfrac, (((320 << hires)>>detailshift)/2)<<FRACBITS);
    }
    else
    {
        projection = centerxfrac;
    }


    if (!detailshift)
    {
        colfunc = basecolfunc = R_DrawColumn;
        tlcolfunc = R_DrawTLColumn;
        transcolfunc = R_DrawTranslatedColumn;
        spanfunc = R_DrawSpan;
    }
    else
    {
        colfunc = basecolfunc = R_DrawColumnLow;
        tlcolfunc = R_DrawTLColumn;
        transcolfunc = R_DrawTranslatedColumn;
        spanfunc = R_DrawSpanLow;
    }

    R_InitBuffer(scaledviewwidth, viewheight);

    R_InitTextureMapping();

//
// psprite scales
//
    pspritescale = FRACUNIT * viewwidth / origwidth;
    pspriteiscale = FRACUNIT * origwidth / viewwidth;

//
// thing clipping
//
    for (i = 0; i < viewwidth; i++)
        screenheightarray[i] = viewheight;

//
// planes
//
    for (i = 0; i < viewheight; i++)
    {
        const fixed_t num_wide = MIN(viewwidth<<detailshift, 320 << !detailshift)/2*FRACUNIT;
        const fixed_t num = (viewwidth<<(detailshift && !hires))/2*FRACUNIT;

        for (j = 0; j < LOOKDIRS; j++)
        {
            if (aspect_ratio >= 2)
            {
                dy = ((i-(viewheight/2 + ((j-LOOKDIRMIN) << (hires && !detailshift)) 
                   * (screenblocks < 9 ? screenblocks : 9) / 10))<<FRACBITS)+FRACUNIT/2;
            }
            else
            {
                dy = ((i-(viewheight/2 + ((j-LOOKDIRMIN) << (hires && !detailshift))
                   * (screenblocks < 11 ? screenblocks : 11) / 10))<<FRACBITS)+FRACUNIT/2;
            }

        dy = abs(dy);
        yslopes[j][i] = FixedDiv (aspect_ratio >= 2 ? num_wide : num, dy);
        }
    }
    yslope = yslopes[LOOKDIRMIN];

    for (i = 0; i < viewwidth; i++)
    {
        cosadj = abs(finecosine[xtoviewangle[i] >> ANGLETOFINESHIFT]);
        distscale[i] = FixedDiv(FRACUNIT, cosadj);
    }

//
// Calculate the light levels to use for each level / scale combination
//
    for (i = 0; i < LIGHTLEVELS; i++)
    {
        startmap = ((LIGHTLEVELS - 1 - i) * 2) * NUMCOLORMAPS / LIGHTLEVELS;
        for (j = 0; j < MAXLIGHTSCALE; j++)
        {
            level =
                startmap -
                j * screenwidth / (viewwidth << detailshift) / DISTMAP;
            if (level < 0)
                level = 0;
            if (level >= NUMCOLORMAPS)
                level = NUMCOLORMAPS - 1;
            scalelight[i][j] = colormaps + level * 256;
            
            // [JN] Brightmaps
            fullbright_greenonly[i][j] = brightmaps_greenonly + level * 256;
            fullbright_redonly[i][j] = brightmaps_redonly + level * 256;
            fullbright_blueonly[i][j] = brightmaps_blueonly + level * 256;
            fullbright_purpleonly[i][j] = brightmaps_purpleonly + level * 256;
            fullbright_notbronze[i][j] = brightmaps_notbronze + level * 256;
            fullbright_flame[i][j] = brightmaps_flame + level * 256;
            fullbright_greenonly_dim[i][j] = brightmaps_greenonly_dim + level * 256;
            fullbright_redonly_dim[i][j] = brightmaps_redonly_dim + level * 256;
            fullbright_blueonly_dim[i][j] = brightmaps_blueonly_dim + level * 256;
            fullbright_yellowonly_dim[i][j] = brightmaps_yellowonly_dim + level * 256;
            fullbright_ethereal[i][j] = brightmaps_ethereal + level * 256;
        }
    }

    // [crispy] lookup table for horizontal screen coordinates
    for (i = 0, j = scaledviewwidth - 1; i < scaledviewwidth; i++, j--)
    {
        flipwidth[i] = flip_levels ? j : i;
    }

    // [JN] Skip weapon bobbing interpolation for next frame.
    skippsprinterp = true;

//
// draw the border
//
    R_DrawViewBorder();         // erase old menu stuff
}


/*
==============
=
= R_Init
=
==============
*/

int detailLevel;
int screenblocks = 10;

void R_Init(void)
{
    if (aspect_ratio >= 2)
    {
        // [JN] Wide screen: don't allow unsupported view modes at startup
        if (screenblocks < 9)
            screenblocks = 9;
        if (screenblocks > 12)
            screenblocks = 12;
    }

    R_InitData();
    printf (".");
    R_InitPointToAngle();
    printf (".");
    R_InitTables();
    // viewwidth / viewheight / detailLevel are set by the defaults
    printf (".");
    R_SetViewSize(screenblocks, detailLevel);
    R_InitPlanes();
    printf (".");
    R_InitLightTables();
    printf (".");
    R_InitSkyMap();
    printf (".");
    R_InitTranslationTables();

    framecount = 0;
}


/*
==============
=
= R_PointInSubsector
=
==============
*/

subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
    node_t *node;
    int side, nodenum;

    if (!numnodes)              // single subsector is a special case
        return subsectors;

    nodenum = numnodes - 1;

    while (!(nodenum & NF_SUBSECTOR))
    {
        node = &nodes[nodenum];
        side = R_PointOnSide(x, y, node);
        nodenum = node->children[side];
    }

    return &subsectors[nodenum & ~NF_SUBSECTOR];

}

//----------------------------------------------------------------------------
//
// PROC R_SetupFrame
//
//----------------------------------------------------------------------------

void R_SetupFrame(player_t * player)
{
    int i;
    int tempCentery;
    int pitch;

    //drawbsp = 1;
    viewplayer = player;
    // haleyjd: removed WATCOMC
    // haleyjd FIXME: viewangleoffset handling?

    // [AM] Interpolate the player camera if the feature is enabled.

    if (uncapped_fps && !vanillaparm &&
        // Don't interpolate on the first tic of a level,
        // otherwise oldviewz might be garbage.
        leveltime > 1 &&
        // Don't interpolate if the player did something
        // that would necessitate turning it off for a tic.
        player->mo->interp == true &&
        // Don't interpolate during a paused state
        !paused && (!menuactive || demoplayback || netgame))
    {
        // Interpolate player camera from their old position to their current one.
        // [JN] Also for player's chicken mode.
        if (player->chickenTics && player->chickenPeck)
        {
            viewx = player->mo->oldx + FixedMul(player->mo->x
                                              - player->mo->oldx, fractionaltic) 
                  + player->chickenPeck * finecosine[viewangle>>ANGLETOFINESHIFT];
            viewy = player->mo->oldy + FixedMul(player->mo->y 
                                              - player->mo->oldy, fractionaltic)
                  + player->chickenPeck * finesine[viewangle>>ANGLETOFINESHIFT];
        }
        else
        {
            viewx = player->mo->oldx + FixedMul(player->mo->x
                                              - player->mo->oldx, fractionaltic);
            viewy = player->mo->oldy + FixedMul(player->mo->y -
                                                player->mo->oldy, fractionaltic);
        }
        viewz = player->oldviewz + FixedMul(player->viewz
                                          - player->oldviewz, fractionaltic);
        viewangle = R_InterpolateAngle(player->mo->oldangle,
                                       player->mo->angle, fractionaltic)
                                       + viewangleoffset;

        pitch = (player->oldlookdir + (player->lookdir - player->oldlookdir)
                                    * FIXED2DOUBLE(fractionaltic)) / MLOOKUNIT;
    }
    else
    {                           // Normal view position
        viewx = player->mo->x;
        viewy = player->mo->y;
        viewz = player->viewz;
        viewangle = player->mo->angle + viewangleoffset;

        // [crispy] pitch is actual lookdir /*and weapon pitch*/
        pitch = player->lookdir / MLOOKUNIT;
    }

    extralight = player->extralight;

    if (pitch > LOOKDIRMAX)
    pitch = LOOKDIRMAX;
    else
    if (pitch < -LOOKDIRMIN)
    pitch = -LOOKDIRMIN;

    // apply new yslope[] whenever "lookdir", "detailshift" or "screenblocks" change
    if (aspect_ratio >= 2)
    {
        tempCentery = viewheight/2 + (pitch << (hires && !detailshift))
                                   * (screenblocks < 9 ? screenblocks : 9) / 10;
    }
    else
    {
        tempCentery = viewheight/2 + (pitch << (hires && !detailshift))
                                   * (screenblocks < 11 ? screenblocks : 11) / 10;
    }

    if (centery != tempCentery)
    {
        centery = tempCentery;
        centeryfrac = centery << FRACBITS;
        yslope = yslopes[LOOKDIRMIN + pitch];
    }
    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
    sscount = 0;
    if (player->fixedcolormap)
    {
        fixedcolormap = colormaps + player->fixedcolormap
            * 256 * sizeof(lighttable_t);
        walllights = scalelightfixed;
        for (i = 0; i < MAXLIGHTSCALE; i++)
        {
            scalelightfixed[i] = fixedcolormap;
        }
    }
    else
    {
        fixedcolormap = 0;
    }
    framecount++;
    validcount++;
    if (BorderNeedRefresh)
    {
        if (setblocks < 10)
        {
            R_DrawViewBorder();
        }
        BorderNeedRefresh = false;
        BorderTopRefresh = false;
        UpdateState |= I_FULLSCRN;
    }
    if (BorderTopRefresh)
    {
        if (setblocks < 10)
        {
            R_DrawTopBorder();
        }
        BorderTopRefresh = false;
        UpdateState |= I_MESSAGES;
    }
}

/*
==============
=
= R_RenderView
=
==============
*/

void R_RenderPlayerView(player_t * player)
{
    extern void V_DrawFilledBox (int x, int y, int w, int h, int c);
    extern void R_InterpolateTextureOffsets (void);
    extern boolean automapactive;

    R_SetupFrame(player);
    R_ClearClipSegs();
    R_ClearDrawSegs();
    if (automapactive && !automap_overlay)
    {
        R_RenderBSPNode (numnodes-1);
        return;
    }
    // [JN] Fill level's "out of bounds" with black color
    V_DrawFilledBox(viewwindowx, viewwindowy, scaledviewwidth, viewheight, 0);

    R_ClearPlanes();
    R_ClearSprites();
    NetUpdate();                // check for new console commands
    R_InterpolateTextureOffsets();      // [crispy] smooth texture scrolling
    R_RenderBSPNode(numnodes - 1);      // the head node is the last node output
    NetUpdate();                // check for new console commands
    R_DrawPlanes();
    NetUpdate();                // check for new console commands
    R_DrawMasked();
    NetUpdate();                // check for new console commands
}
