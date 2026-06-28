/*
 * geochg – Geometry Change animation plugin for Compiz 0.8
 *
 * Smoothly animates window geometry changes (resize + reposition), similar to
 * KWin's "Geometry Change" effect.  Triggered by windowResizeNotify, which
 * fires for maximise, snap-tiling, and any programmatic resize, and carries
 * both position (dx/dy) and size (dwidth/dheight) deltas.
 */

#include <stdlib.h>
#include <math.h>
#include <compiz-core.h>
#include "geochg_options.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Full painted geometry (includes shadow/output extents). */
#define GEOCHG_WIN_X(w) ((w)->attrib.x - (w)->output.left)
#define GEOCHG_WIN_Y(w) ((w)->attrib.y - (w)->output.top)
#define GEOCHG_WIN_W(w) ((w)->width  + (w)->output.left + (w)->output.right)
#define GEOCHG_WIN_H(w) ((w)->height + (w)->output.top  + (w)->output.bottom)

static int corePrivateIndex;
static int displayPrivateIndex;

typedef struct _GeochgCore    { ObjectAddProc objectAdd; } GeochgCore;
typedef struct _GeochgDisplay { int screenPrivateIndex;  } GeochgDisplay;

typedef struct _GeochgScreen {
    int windowPrivateIndex;

    PreparePaintScreenProc preparePaintScreen;
    DonePaintScreenProc    donePaintScreen;
    PaintWindowProc        paintWindow;
    WindowResizeNotifyProc windowResizeNotify;

    Bool anyAnimating;
} GeochgScreen;

typedef struct _GeochgWindow {
    Bool  animActive;
    float oldX, oldY, oldW, oldH; /* WIN_ geometry at animation start   */
    float animTotal;               /* total duration (ms)                 */
    float animRemaining;           /* time left (ms)                      */
} GeochgWindow;

/* ---- Private-data accessors ------------------------------------------ */

#define GET_GEOCHG_CORE(c) \
    ((GeochgCore *) (c)->base.privates[corePrivateIndex].ptr)
#define GEOCHG_CORE(c) \
    GeochgCore *gc = GET_GEOCHG_CORE (c)

#define GET_GEOCHG_DISPLAY(d) \
    ((GeochgDisplay *) (d)->base.privates[displayPrivateIndex].ptr)
#define GEOCHG_DISPLAY(d) \
    GeochgDisplay *gd = GET_GEOCHG_DISPLAY (d)

#define GET_GEOCHG_SCREEN(s, gd) \
    ((GeochgScreen *) (s)->base.privates[(gd)->screenPrivateIndex].ptr)
#define GEOCHG_SCREEN(s) \
    GeochgDisplay *gd = GET_GEOCHG_DISPLAY ((s)->display); \
    GeochgScreen  *gs = GET_GEOCHG_SCREEN  (s, gd)

#define GET_GEOCHG_WINDOW(w, gs) \
    ((GeochgWindow *) (w)->base.privates[(gs)->windowPrivateIndex].ptr)
#define GEOCHG_WINDOW(w) \
    GEOCHG_SCREEN (w->screen); \
    GeochgWindow *aw = GET_GEOCHG_WINDOW (w, gs)

/* ---- Forward declarations ------------------------------------------------ */

static void geochgDamageWindow (CompWindow *w);

/* ---- Helpers ------------------------------------------------------------- */

/* Ease-out cubic: fast start, decelerates to rest. */
static float
easeOutCubic (float t)
{
    float f = 1.0f - t;
    return 1.0f - f * f * f;
}

static float
clamp01 (float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Skip windows that shouldn't be animated (desktop, panels, tiny). */
static Bool
shouldAnimate (CompWindow *w)
{
    if (w->type & (CompWindowTypeDesktopMask | CompWindowTypeDockMask))
        return FALSE;
    if (w->attrib.class == InputOnly)
        return FALSE;
    if (w->width < 4 || w->height < 4)
        return FALSE;
    return TRUE;
}

/* ---- Hooks --------------------------------------------------------------- */

static void
geochgWindowResizeNotify (CompWindow *w,
                          int         dx,
                          int         dy,
                          int         dwidth,
                          int         dheight)
{
    GEOCHG_WINDOW (w);

    if (shouldAnimate (w) && (dx || dy || dwidth || dheight))
    {
        /*
         * At this point w already has the NEW geometry.
         * Reconstruct old WIN_ coordinates from the deltas.
         */
        float prevX = (float) GEOCHG_WIN_X (w) - dx;
        float prevY = (float) GEOCHG_WIN_Y (w) - dy;
        float prevW = (float) GEOCHG_WIN_W (w) - dwidth;
        float prevH = (float) GEOCHG_WIN_H (w) - dheight;

        if (prevW >= 1.0f && prevH >= 1.0f)
        {
            if (aw->animActive && aw->animTotal > 0.0f)
            {
                /*
                 * A new resize interrupted an on-going animation.
                 * Start the new animation from the current apparent geometry
                 * so there is no visual jump.
                 */
                float rawProg = 1.0f - aw->animRemaining / aw->animTotal;
                float t = easeOutCubic (clamp01 (rawProg));
                aw->oldX = aw->oldX + (prevX - aw->oldX) * t;
                aw->oldY = aw->oldY + (prevY - aw->oldY) * t;
                aw->oldW = aw->oldW + (prevW - aw->oldW) * t;
                aw->oldH = aw->oldH + (prevH - aw->oldH) * t;
            }
            else
            {
                aw->oldX = prevX;
                aw->oldY = prevY;
                aw->oldW = prevW;
                aw->oldH = prevH;
            }

            aw->animTotal     = (float) geochgGetDuration (w->screen);
            aw->animRemaining = aw->animTotal;
            aw->animActive    = TRUE;
            gs->anyAnimating  = TRUE;

            geochgDamageWindow (w); /* kick off first frame (old ∪ new area) */
        }
    }

    UNWRAP (gs, w->screen, windowResizeNotify);
    (*w->screen->windowResizeNotify) (w, dx, dy, dwidth, dheight);
    WRAP (gs, w->screen, windowResizeNotify, geochgWindowResizeNotify);
}

/*
 * Damage only the area the animation can touch: the union of the old and the
 * new window geometry (both already include output/shadow extents).  This
 * avoids repainting the whole desktop every frame, which is the expensive part
 * on slower GPUs.  The window texture itself is not re-rendered – only the
 * already-drawn texture is re-composited with a transform.
 */
static void
geochgDamageWindow (CompWindow *w)
{
    GEOCHG_WINDOW (w);
    REGION reg;

    float newX = (float) GEOCHG_WIN_X (w);
    float newY = (float) GEOCHG_WIN_Y (w);
    float newW = (float) GEOCHG_WIN_W (w);
    float newH = (float) GEOCHG_WIN_H (w);

    float x1 = aw->oldX < newX ? aw->oldX : newX;
    float y1 = aw->oldY < newY ? aw->oldY : newY;
    float x2 = (aw->oldX + aw->oldW) > (newX + newW) ?
               (aw->oldX + aw->oldW) : (newX + newW);
    float y2 = (aw->oldY + aw->oldH) > (newY + newH) ?
               (aw->oldY + aw->oldH) : (newY + newH);

    reg.rects    = &reg.extents;
    reg.numRects = 1;
    reg.extents.x1 = (int) x1 - 1;
    reg.extents.y1 = (int) y1 - 1;
    reg.extents.x2 = (int) x2 + 1;
    reg.extents.y2 = (int) y2 + 1;

    damageScreenRegion (w->screen, &reg);
}

static void
geochgPreparePaintScreen (CompScreen *s, int msSinceLastPaint)
{
    GEOCHG_SCREEN (s);

    if (gs->anyAnimating)
    {
        CompWindow *w;

        /* Advance every active animation by the elapsed time. */
        for (w = s->windows; w; w = w->next)
        {
            GeochgWindow *aw = GET_GEOCHG_WINDOW (w, gs);

            if (aw->animActive)
            {
                aw->animRemaining -= (float) msSinceLastPaint;
                if (aw->animRemaining <= 0.0f)
                {
                    aw->animRemaining = 0.0f;
                    aw->animActive    = FALSE;
                }
            }
        }
    }

    UNWRAP (gs, s, preparePaintScreen);
    (*s->preparePaintScreen) (s, msSinceLastPaint);
    WRAP (gs, s, preparePaintScreen, geochgPreparePaintScreen);
}

static void
geochgDonePaintScreen (CompScreen *s)
{
    GEOCHG_SCREEN (s);

    if (gs->anyAnimating)
    {
        CompWindow *w;
        gs->anyAnimating = FALSE;

        /*
         * Damage the animated region *after* painting so the next frame is
         * scheduled.  Doing this in donePaintScreen (not preparePaintScreen)
         * is what keeps the animation self-sustaining: damage added here is
         * processed on the following cycle.
         */
        for (w = s->windows; w; w = w->next)
        {
            GeochgWindow *aw = GET_GEOCHG_WINDOW (w, gs);

            if (aw->animActive)
            {
                geochgDamageWindow (w);
                gs->anyAnimating = TRUE;
            }
        }
    }

    UNWRAP (gs, s, donePaintScreen);
    (*s->donePaintScreen) (s);
    WRAP (gs, s, donePaintScreen, geochgDonePaintScreen);
}

static Bool
geochgPaintWindow (CompWindow              *w,
                   const WindowPaintAttrib *attrib,
                   const CompTransform     *transform,
                   Region                   region,
                   unsigned int             mask)
{
    GEOCHG_WINDOW (w);
    Bool status;

    if (aw->animActive && aw->animTotal > 0.0f)
    {
        float newX = (float) GEOCHG_WIN_X (w);
        float newY = (float) GEOCHG_WIN_Y (w);
        float newW = (float) GEOCHG_WIN_W (w);
        float newH = (float) GEOCHG_WIN_H (w);

        if (newW >= 1.0f && newH >= 1.0f &&
            aw->oldW >= 1.0f && aw->oldH >= 1.0f)
        {
            float rawProg = 1.0f - aw->animRemaining / aw->animTotal;
            float t = easeOutCubic (clamp01 (rawProg));

            /* Interpolated geometry (position + size). */
            float lerpX = aw->oldX + (newX - aw->oldX) * t;
            float lerpY = aw->oldY + (newY - aw->oldY) * t;
            float lerpW = aw->oldW + (newW - aw->oldW) * t;
            float lerpH = aw->oldH + (newH - aw->oldH) * t;

            /* Scale factors relative to the window's actual (new) size. */
            float sx = lerpW / newW;
            float sy = lerpH / newH;

            /* Real painted centre vs. desired interpolated centre. */
            float realCX = newX + newW * 0.5f;
            float realCY = newY + newH * 0.5f;
            float lerpCX = lerpX + lerpW * 0.5f;
            float lerpCY = lerpY + lerpH * 0.5f;

            CompTransform wTransform = *transform;

            /*
             * Build the scale-about-a-point transform:
             *   1. Shift real centre to origin
             *   2. Scale
             *   3. Shift to interpolated centre
             * matrixTranslate/Scale post-multiply, so the matrix is applied
             * right-to-left on each vertex.  Opacity is left untouched – only
             * the geometry of the final drawn texture is smoothed.
             */
            matrixTranslate (&wTransform, lerpCX,  lerpCY,  0.0f);
            matrixScale     (&wTransform, sx, sy, 1.0f);
            matrixTranslate (&wTransform, -realCX, -realCY, 0.0f);

            mask |= PAINT_WINDOW_TRANSFORMED_MASK;

            UNWRAP (gs, w->screen, paintWindow);
            status = (*w->screen->paintWindow) (w, attrib, &wTransform,
                                                region, mask);
            WRAP (gs, w->screen, paintWindow, geochgPaintWindow);

            return status;
        }
    }

    UNWRAP (gs, w->screen, paintWindow);
    status = (*w->screen->paintWindow) (w, attrib, transform, region, mask);
    WRAP (gs, w->screen, paintWindow, geochgPaintWindow);
    return status;
}

/* ---- Per-object init / fini ---------------------------------------------- */

static Bool
GeochgInitCore (CompPlugin *p, CompCore *c)
{
    GeochgCore *gc;

    if (!checkPluginABI ("core", CORE_ABIVERSION))
        return FALSE;

    gc = calloc (1, sizeof (*gc));
    if (!gc)
        return FALSE;

    displayPrivateIndex = allocateDisplayPrivateIndex ();
    if (displayPrivateIndex < 0) {
        free (gc);
        return FALSE;
    }

    c->base.privates[corePrivateIndex].ptr = gc;
    return TRUE;
}

static void
GeochgFiniCore (CompPlugin *p, CompCore *c)
{
    GEOCHG_CORE (c);
    freeDisplayPrivateIndex (displayPrivateIndex);
    free (gc);
}

static Bool
GeochgInitDisplay (CompPlugin *p, CompDisplay *d)
{
    GeochgDisplay *gd = calloc (1, sizeof (*gd));
    if (!gd)
        return FALSE;

    gd->screenPrivateIndex = allocateScreenPrivateIndex (d);
    if (gd->screenPrivateIndex < 0) {
        free (gd);
        return FALSE;
    }

    d->base.privates[displayPrivateIndex].ptr = gd;
    return TRUE;
}

static void
GeochgFiniDisplay (CompPlugin *p, CompDisplay *d)
{
    GEOCHG_DISPLAY (d);
    freeScreenPrivateIndex (d, gd->screenPrivateIndex);
    free (gd);
}

static Bool
GeochgInitScreen (CompPlugin *p, CompScreen *s)
{
    GeochgDisplay *gd = GET_GEOCHG_DISPLAY (s->display);
    GeochgScreen  *gs = calloc (1, sizeof (*gs));
    if (!gs)
        return FALSE;

    gs->windowPrivateIndex = allocateWindowPrivateIndex (s);
    if (gs->windowPrivateIndex < 0) {
        free (gs);
        return FALSE;
    }

    gs->anyAnimating = FALSE;

    WRAP (gs, s, preparePaintScreen, geochgPreparePaintScreen);
    WRAP (gs, s, donePaintScreen,    geochgDonePaintScreen);
    WRAP (gs, s, paintWindow,        geochgPaintWindow);
    WRAP (gs, s, windowResizeNotify, geochgWindowResizeNotify);

    s->base.privates[gd->screenPrivateIndex].ptr = gs;
    return TRUE;
}

static void
GeochgFiniScreen (CompPlugin *p, CompScreen *s)
{
    GEOCHG_SCREEN (s);

    UNWRAP (gs, s, preparePaintScreen);
    UNWRAP (gs, s, donePaintScreen);
    UNWRAP (gs, s, paintWindow);
    UNWRAP (gs, s, windowResizeNotify);

    freeWindowPrivateIndex (s, gs->windowPrivateIndex);
    free (gs);
}

static Bool
GeochgInitWindow (CompPlugin *p, CompWindow *w)
{
    GEOCHG_SCREEN (w->screen);
    GeochgWindow *aw = calloc (1, sizeof (*aw));
    if (!aw)
        return FALSE;

    w->base.privates[gs->windowPrivateIndex].ptr = aw;
    return TRUE;
}

static void
GeochgFiniWindow (CompPlugin *p, CompWindow *w)
{
    GEOCHG_SCREEN (w->screen);
    free (GET_GEOCHG_WINDOW (w, gs));
}

/* ---- Plugin entry points ------------------------------------------------- */

static Bool
GeochgInit (CompPlugin *p)
{
    corePrivateIndex = allocateCorePrivateIndex ();
    return corePrivateIndex >= 0;
}

static void
GeochgFini (CompPlugin *p)
{
    freeCorePrivateIndex (corePrivateIndex);
}

static CompBool
GeochgInitObject (CompPlugin *p, CompObject *o)
{
    static InitPluginObjectProc dispTab[] = {
        (InitPluginObjectProc) GeochgInitCore,
        (InitPluginObjectProc) GeochgInitDisplay,
        (InitPluginObjectProc) GeochgInitScreen,
        (InitPluginObjectProc) GeochgInitWindow
    };
    RETURN_DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), TRUE, (p, o));
}

static void
GeochgFiniObject (CompPlugin *p, CompObject *o)
{
    static FiniPluginObjectProc dispTab[] = {
        (FiniPluginObjectProc) GeochgFiniCore,
        (FiniPluginObjectProc) GeochgFiniDisplay,
        (FiniPluginObjectProc) GeochgFiniScreen,
        (FiniPluginObjectProc) GeochgFiniWindow
    };
    DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), (p, o));
}

CompPluginVTable GeochgVTable = {
    "geochg",
    0,
    GeochgInit,
    GeochgFini,
    GeochgInitObject,
    GeochgFiniObject,
    0,
    0,
};

CompPluginVTable *
getCompPluginInfo (void)
{
    return &GeochgVTable;
}
