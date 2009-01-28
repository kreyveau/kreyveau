/*
 * Copyright 2003 NVIDIA, Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nv_include.h"
#include "nv_rop.h"

static void 
NV04EXASetPattern(ScrnInfoPtr pScrn, CARD32 clr0, CARD32 clr1,
		  CARD32 pat0, CARD32 pat1)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *patt = pNv->NvImagePattern;

	BEGIN_RING(chan, patt, NV04_IMAGE_PATTERN_MONOCHROME_COLOR0, 4);
	OUT_RING  (chan, clr0);
	OUT_RING  (chan, clr1);
	OUT_RING  (chan, pat0);
	OUT_RING  (chan, pat1);
}

static void 
NV04EXASetROP(ScrnInfoPtr pScrn, CARD32 alu, CARD32 planemask)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *rop = pNv->NvRop;
	
	if (planemask != ~0) {
		NV04EXASetPattern(pScrn, 0, planemask, ~0, ~0);
		if (pNv->currentRop != (alu + 32)) {
			BEGIN_RING(chan, rop, NV03_CONTEXT_ROP_ROP, 1);
			OUT_RING  (chan, NVROP[alu].copy_planemask);
			pNv->currentRop = alu + 32;
		}
	} else
	if (pNv->currentRop != alu) {
		if(pNv->currentRop >= 16)
			NV04EXASetPattern(pScrn, ~0, ~0, ~0, ~0);
		BEGIN_RING(chan, rop, NV03_CONTEXT_ROP_ROP, 1);
		OUT_RING  (chan, NVROP[alu].copy);
		pNv->currentRop = alu;
	}
}

static void
NV04EXAStateSolidResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);

	NV04EXAPrepareSolid(pNv->pdpix, pNv->alu, pNv->planemask, pNv->fg_colour);
}

Bool
NV04EXAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *surf2d = pNv->NvContextSurfaces;
	struct nouveau_grobj *rect = pNv->NvRectangle;
	unsigned int fmt, pitch, color;

	WAIT_RING(chan, 64);

	planemask |= ~0 << pPixmap->drawable.bitsPerPixel;
	if (planemask != ~0 || alu != GXcopy) {
		if (pPixmap->drawable.bitsPerPixel == 32)
			return FALSE;
		BEGIN_RING(chan, rect, NV04_GDI_RECTANGLE_TEXT_OPERATION, 1);
		OUT_RING  (chan, 1); /* ROP_AND */
		NV04EXASetROP(pScrn, alu, planemask);
	} else {
		BEGIN_RING(chan, rect, NV04_GDI_RECTANGLE_TEXT_OPERATION, 1);
		OUT_RING  (chan, 3); /* SRCCOPY */
	}

	if (!NVAccelGetCtxSurf2DFormatFromPixmap(pPixmap, (int*)&fmt))
		return FALSE;
	pitch = exaGetPixmapPitch(pPixmap);

	if (pPixmap->drawable.bitsPerPixel == 16) {
		/* convert to 32bpp */
		uint32_t r =  (fg&0x1F)          * 255 / 31;
		uint32_t g = ((fg&0x7E0) >> 5)   * 255 / 63;
		uint32_t b = ((fg&0xF100) >> 11) * 255 / 31;
		color = b<<16 | g<<8 | r;
	} else 
		color = fg;

	/* When SURFACE_FORMAT_A8R8G8B8 is used with GDI_RECTANGLE_TEXT, the 
	 * alpha channel gets forced to 0xFF for some reason.  We're using 
	 * SURFACE_FORMAT_Y32 as a workaround
	 */
	if (fmt == NV04_CONTEXT_SURFACES_2D_FORMAT_A8R8G8B8)
		fmt = NV04_CONTEXT_SURFACES_2D_FORMAT_Y32;

	BEGIN_RING(chan, surf2d, NV04_CONTEXT_SURFACES_2D_FORMAT, 4);
	OUT_RING  (chan, fmt);
	OUT_RING  (chan, (pitch << 16) | pitch);
	OUT_PIXMAPl(chan, pPixmap, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR);
	OUT_PIXMAPl(chan, pPixmap, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR);

	BEGIN_RING(chan, rect, NV04_GDI_RECTANGLE_TEXT_COLOR_FORMAT, 1);
	OUT_RING  (chan, NV04_GDI_RECTANGLE_TEXT_COLOR_FORMAT_A8R8G8B8);
	BEGIN_RING(chan, rect, NV04_GDI_RECTANGLE_TEXT_COLOR1_A, 1);
	OUT_RING (chan, color);

	pNv->pdpix = pPixmap;
	pNv->alu = alu;
	pNv->planemask = planemask;
	pNv->fg_colour = fg;
	chan->flush_notify = NV04EXAStateSolidResubmit;
	return TRUE;
}

void
NV04EXASolid (PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *rect = pNv->NvRectangle;
	int width = x2-x1;
	int height = y2-y1;

	WAIT_RING (chan, 3);
	BEGIN_RING(chan, rect,
		   NV04_GDI_RECTANGLE_TEXT_UNCLIPPED_RECTANGLE_POINT(0), 2);
	OUT_RING  (chan, (x1 << 16) | y1);
	OUT_RING  (chan, (width << 16) | height);

	if((width * height) >= 512)
		FIRE_RING (chan);
}

void
NV04EXADoneSolid (PixmapPtr pPixmap)
{
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);

	pNv->chan->flush_notify = NULL;
}

static void
NV04EXAStateCopyResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);

	NV04EXAPrepareCopy(pNv->pspix, pNv->pdpix, 0, 0, pNv->alu,
			   pNv->planemask);
}

Bool
NV04EXAPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int dx, int dy,
		   int alu, Pixel planemask)
{
	ScrnInfoPtr pScrn = xf86Screens[pSrcPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *surf2d = pNv->NvContextSurfaces;
	struct nouveau_grobj *blit = pNv->NvImageBlit;
	int fmt;

	WAIT_RING(chan, 64);

	if (pSrcPixmap->drawable.bitsPerPixel !=
			pDstPixmap->drawable.bitsPerPixel)
		return FALSE;

	planemask |= ~0 << pDstPixmap->drawable.bitsPerPixel;
	if (planemask != ~0 || alu != GXcopy) {
		if (pDstPixmap->drawable.bitsPerPixel == 32)
			return FALSE;
		BEGIN_RING(chan, blit, NV04_IMAGE_BLIT_OPERATION, 1);
		OUT_RING  (chan, 1); /* ROP_AND */
		NV04EXASetROP(pScrn, alu, planemask);
	} else {
		BEGIN_RING(chan, blit, NV04_IMAGE_BLIT_OPERATION, 1);
		OUT_RING  (chan, 3); /* SRCCOPY */
	}

	if (!NVAccelGetCtxSurf2DFormatFromPixmap(pDstPixmap, &fmt))
		return FALSE;

	BEGIN_RING(chan, surf2d, NV04_CONTEXT_SURFACES_2D_FORMAT, 4);
	OUT_RING  (chan, fmt);
	OUT_RING  (chan, (exaGetPixmapPitch(pDstPixmap) << 16) |
		   (exaGetPixmapPitch(pSrcPixmap)));
	OUT_PIXMAPl(chan, pSrcPixmap, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_RD);
	OUT_PIXMAPl(chan, pDstPixmap, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR);

	pNv->pspix = pSrcPixmap;
	pNv->pdpix = pDstPixmap;
	pNv->alu = alu;
	pNv->planemask = planemask;
	chan->flush_notify = NV04EXAStateCopyResubmit;
	return TRUE;
}

void
NV04EXACopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY,
	    int width, int height)
{
	ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *blit = pNv->NvImageBlit;

	WAIT_RING (chan, 4);
	BEGIN_RING(chan, blit, NV01_IMAGE_BLIT_POINT_IN, 3);
	OUT_RING  (chan, (srcY << 16) | srcX);
	OUT_RING  (chan, (dstY << 16) | dstX);
	OUT_RING  (chan, (height  << 16) | width);

	if((width * height) >= 512)
		FIRE_RING (chan);
}

void
NV04EXADoneCopy(PixmapPtr pDstPixmap)
{
	ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);

	pNv->chan->flush_notify = NULL;
}

static void
NV04EXAStateIFCResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_grobj *surf2d = pNv->NvContextSurfaces;
	struct nouveau_grobj *ifc = pNv->NvImageFromCpu;
	int surf_fmt;

	NVAccelGetCtxSurf2DFormatFromPixmap(pNv->pdpix, &surf_fmt);

	BEGIN_RING(chan, surf2d, NV04_CONTEXT_SURFACES_2D_FORMAT, 4);
	OUT_RING  (chan, surf_fmt);
	OUT_RING  (chan, (exaGetPixmapPitch(pNv->pdpix) << 16) |
			  exaGetPixmapPitch(pNv->pdpix));
	OUT_PIXMAPl(chan, pNv->pdpix, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR);
	OUT_PIXMAPl(chan, pNv->pdpix, 0, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR);
	BEGIN_RING(chan, ifc, NV01_IMAGE_FROM_CPU_POINT, 3);
	OUT_RING  (chan, (pNv->point_y << 16) | pNv->point_x);
	OUT_RING  (chan, (pNv->height_out << 16) | pNv->width_out);
	OUT_RING  (chan, (pNv->height_in << 16) | pNv->width_in);
}

Bool
NV04EXAUploadIFC(ScrnInfoPtr pScrn, const char *src, int src_pitch,
		 PixmapPtr pDst, int x, int y, int w, int h, int cpp)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *clip = pNv->NvClipRectangle;
	struct nouveau_grobj *ifc = pNv->NvImageFromCpu;
	int line_len = w * cpp;
	int iw, id, surf_fmt, ifc_fmt;
	int padbytes;

	if (pNv->Architecture >= NV_ARCH_50)
		return FALSE;

	if (h > 1024)
		return FALSE;

	if (line_len<4)
		return FALSE;

	switch (cpp) {
	case 2: ifc_fmt = 1; break;
	case 4: ifc_fmt = 4; break;
	default:
		return FALSE;
	}

	if (!NVAccelGetCtxSurf2DFormatFromPixmap(pDst, &surf_fmt))
		return FALSE;

	/* Pad out input width to cover both COLORA() and COLORB() */
	iw  = (line_len + 7) & ~7;
	padbytes = iw - line_len;
	id  = iw / 4; /* line push size */
	iw /= cpp;

	/* Don't support lines longer than max push size yet.. */
	if (id > 1792)
		return FALSE;

	BEGIN_RING(chan, clip, NV01_CONTEXT_CLIP_RECTANGLE_POINT, 2);
	OUT_RING  (chan, 0x0); 
	OUT_RING  (chan, 0x7FFF7FFF);
	BEGIN_RING(chan, ifc, NV01_IMAGE_FROM_CPU_OPERATION, 2);
	OUT_RING  (chan, NV01_IMAGE_FROM_CPU_OPERATION_SRCCOPY);
	OUT_RING  (chan, ifc_fmt);

	pNv->point_x = x;
	pNv->point_y = y;
	pNv->height_in = pNv->height_out = h;
	pNv->width_in = iw;
	pNv->width_out = w;
	pNv->pdpix = pDst;
	chan->flush_notify = NV04EXAStateIFCResubmit;
	NV04EXAStateIFCResubmit(chan);

	if (padbytes)
		h--;
	while (h--) {
		/* send a line */
		BEGIN_RING(chan, ifc, NV01_IMAGE_FROM_CPU_COLOR(0), id);
		OUT_RINGp (chan, src, id);

		src += src_pitch;
		pNv->point_y++;
	}
	if (padbytes) {
		char padding[8];
		int aux = (padbytes + 7) >> 2;
		BEGIN_RING(chan, ifc, NV01_IMAGE_FROM_CPU_COLOR(0), id);
		OUT_RINGp (chan, src, id - aux);
		memcpy(padding, src + (id - aux) * 4, padbytes);
		OUT_RINGp (chan, padding, aux);
	}

	chan->flush_notify = NULL;
	return TRUE;
}

