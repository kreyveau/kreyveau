/*
 * Copyright 2009 Nouveau Project
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
#include "exa.h"

static inline Bool
NVAccelMemcpyRect(char *dst, const char *src, int height, int dst_pitch,
		  int src_pitch, int line_len)
{
	if ((src_pitch == line_len) && (src_pitch == dst_pitch)) {
		memcpy(dst, src, line_len*height);
	} else {
		while (height--) {
			memcpy(dst, src, line_len);
			src += src_pitch;
			dst += dst_pitch;
		}
	}

	return TRUE;
}

static inline Bool
NVAccelDownloadM2MF(PixmapPtr pspix, int x, int y, int w, int h,
		    char *dst, unsigned dst_pitch)
{
	ScrnInfoPtr pScrn = xf86Screens[pspix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *m2mf = pNv->NvMemFormat;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pspix);
	unsigned cpp = pspix->drawable.bitsPerPixel / 8;
	unsigned line_len = w * cpp;
	unsigned src_offset = 0, src_pitch = 0, linear = 0;
	/* Maximum DMA transfer */
	unsigned line_count = pNv->GART->size / line_len;

	if (!nv50_style_tiled_pixmap(pspix)) {
		linear     = 1;
		src_pitch  = exaGetPixmapPitch(pspix);
		src_offset += (y * src_pitch) + (x * cpp);
	}

	/* HW limitations */
	if (line_count > 2047)
		line_count = 2047;

	while (h) {
		int i;
		char *src;

		if (line_count > h)
			line_count = h;

		if (MARK_RING(chan, 32, 6))
			return FALSE;

		BEGIN_RING(chan, m2mf, NV04_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
		if (OUT_RELOCo(chan, bo, NOUVEAU_BO_GART | NOUVEAU_BO_VRAM |
			       NOUVEAU_BO_RD) ||
		    OUT_RELOCo(chan, pNv->GART, NOUVEAU_BO_GART |
			       NOUVEAU_BO_WR)) {
			MARK_UNDO(chan);
			return FALSE;
		}

		if (pNv->Architecture >= NV_ARCH_50) {
			if (!linear) {
				BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_IN, 7);
				OUT_RING  (chan, 0);
				OUT_RING  (chan, bo->tile_mode << 4);
				OUT_RING  (chan, pspix->drawable.width * cpp);
				OUT_RING  (chan, pspix->drawable.height);
				OUT_RING  (chan, 1);
				OUT_RING  (chan, 0);
				OUT_RING  (chan, (y << 16) | (x * cpp));
			} else {
				BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_IN, 1);
				OUT_RING  (chan, 1);
			}

			BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_OUT, 1);
			OUT_RING  (chan, 1);

			BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN_HIGH, 2);
			if (OUT_RELOCh(chan, bo, src_offset, NOUVEAU_BO_GART |
				       NOUVEAU_BO_VRAM | NOUVEAU_BO_RD) ||
			    OUT_RELOCh(chan, pNv->GART, 0, NOUVEAU_BO_GART |
				       NOUVEAU_BO_WR)) {
				MARK_UNDO(chan);
				return FALSE;
			}
		}

		BEGIN_RING(chan, m2mf,
			   NV04_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
		if (OUT_RELOCl(chan, bo, src_offset, NOUVEAU_BO_GART |
			       NOUVEAU_BO_VRAM | NOUVEAU_BO_RD) ||
		    OUT_RELOCl(chan, pNv->GART, 0, NOUVEAU_BO_GART |
			       NOUVEAU_BO_WR)) {
			MARK_UNDO(chan);
			return FALSE;
		}
		OUT_RING  (chan, src_pitch);
		OUT_RING  (chan, line_len);
		OUT_RING  (chan, line_len);
		OUT_RING  (chan, line_count);
		OUT_RING  (chan, (1<<8)|1);
		OUT_RING  (chan, 0);

		if (nouveau_bo_map(pNv->GART, NOUVEAU_BO_RD)) {
			MARK_UNDO(chan);
			return FALSE;
		}
		src = pNv->GART->map;
		if (dst_pitch == line_len) {
			memcpy(dst, src, dst_pitch * line_count);
			dst += dst_pitch * line_count;
		} else {
			for (i = 0; i < line_count; i++) {
				memcpy(dst, src, line_len);
				src += line_len;
				dst += dst_pitch;
			}
		}
		nouveau_bo_unmap(pNv->GART);

		if (linear)
			src_offset += line_count * src_pitch;
		h -= line_count;
		y += line_count;
	}

	return TRUE;
}

static inline Bool
NVAccelUploadM2MF(PixmapPtr pdpix, int x, int y, int w, int h,
		  const char *src, int src_pitch)
{
	ScrnInfoPtr pScrn = xf86Screens[pdpix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_channel *chan = pNv->chan;
	struct nouveau_grobj *m2mf = pNv->NvMemFormat;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pdpix);
	unsigned cpp = pdpix->drawable.bitsPerPixel / 8;
	unsigned line_len = w * cpp;
	unsigned dst_offset = 0, dst_pitch = 0, linear = 0;
	/* Maximum DMA transfer */
	unsigned line_count = pNv->GART->size / line_len;

	if (!nv50_style_tiled_pixmap(pdpix)) {
		linear     = 1;
		dst_pitch  = exaGetPixmapPitch(pdpix);
		dst_offset += (y * dst_pitch) + (x * cpp);
	}

	/* HW limitations */
	if (line_count > 2047)
		line_count = 2047;

	while (h) {
		int i;
		char *dst;

		if (line_count > h)
			line_count = h;

		/* Upload to GART */
		if (nouveau_bo_map(pNv->GART, NOUVEAU_BO_WR))
			return FALSE;
		dst = pNv->GART->map;
		if (src_pitch == line_len) {
			memcpy(dst, src, src_pitch * line_count);
			src += src_pitch * line_count;
		} else {
			for (i = 0; i < line_count; i++) {
				memcpy(dst, src, line_len);
				src += src_pitch;
				dst += line_len;
			}
		}
		nouveau_bo_unmap(pNv->GART);

		if (MARK_RING(chan, 32, 6))
			return FALSE;

		BEGIN_RING(chan, m2mf, NV04_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
		if (OUT_RELOCo(chan, pNv->GART, NOUVEAU_BO_GART |
			       NOUVEAU_BO_RD) ||
		    OUT_RELOCo(chan, bo, NOUVEAU_BO_VRAM | NOUVEAU_BO_GART |
			       NOUVEAU_BO_WR)) {
			MARK_UNDO(chan);
			return FALSE;
		}

		if (pNv->Architecture >= NV_ARCH_50) {
			BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_IN, 1);
			OUT_RING  (chan, 1);

			if (!linear) {
				BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_OUT, 7);
				OUT_RING  (chan, 0);
				OUT_RING  (chan, bo->tile_mode << 4);
				OUT_RING  (chan, pdpix->drawable.width * cpp);
				OUT_RING  (chan, pdpix->drawable.height);
				OUT_RING  (chan, 1);
				OUT_RING  (chan, 0);
				OUT_RING  (chan, (y << 16) | (x * cpp));
			} else {
				BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_LINEAR_OUT, 1);
				OUT_RING  (chan, 1);
			}

			BEGIN_RING(chan, m2mf, NV50_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN_HIGH, 2);
			if (OUT_RELOCh(chan, pNv->GART, 0, NOUVEAU_BO_GART |
				       NOUVEAU_BO_RD) ||
			    OUT_RELOCh(chan, bo, dst_offset, NOUVEAU_BO_VRAM |
				       NOUVEAU_BO_GART | NOUVEAU_BO_WR)) {
				MARK_UNDO(chan);
				return FALSE;
			}
		}

		/* DMA to VRAM */
		BEGIN_RING(chan, m2mf,
			   NV04_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
		if (OUT_RELOCl(chan, pNv->GART, 0, NOUVEAU_BO_GART |
			       NOUVEAU_BO_RD) ||
		    OUT_RELOCl(chan, bo, dst_offset, NOUVEAU_BO_VRAM |
			       NOUVEAU_BO_GART | NOUVEAU_BO_WR)) {
			MARK_UNDO(chan);
			return FALSE;
		}
		OUT_RING  (chan, line_len);
		OUT_RING  (chan, dst_pitch);
		OUT_RING  (chan, line_len);
		OUT_RING  (chan, line_count);
		OUT_RING  (chan, (1<<8)|1);
		OUT_RING  (chan, 0);
		FIRE_RING (chan);

		if (linear)
			dst_offset += line_count * dst_pitch;
		h -= line_count;
		y += line_count;
	}

	return TRUE;
}

static int
nouveau_exa_mark_sync(ScreenPtr pScreen)
{
	return 0;
}

static void
nouveau_exa_wait_marker(ScreenPtr pScreen, int marker)
{
}

static Bool
nouveau_exa_prepare_access(PixmapPtr ppix, int index)
{
	struct nouveau_bo *bo = nouveau_pixmap_bo(ppix);
	NVPtr pNv = NVPTR(xf86Screens[ppix->drawable.pScreen->myNum]);

	if (nv50_style_tiled_pixmap(ppix) && !pNv->wfb_enabled)
		return FALSE;
	if (nouveau_bo_map(bo, NOUVEAU_BO_RDWR))
		return FALSE;
	ppix->devPrivate.ptr = bo->map;
	return TRUE;
}

static void
nouveau_exa_finish_access(PixmapPtr ppix, int index)
{
	struct nouveau_bo *bo = nouveau_pixmap_bo(ppix);

	nouveau_bo_unmap(bo);
}

static Bool
nouveau_exa_pixmap_is_offscreen(PixmapPtr ppix)
{
	return nouveau_pixmap_bo(ppix) != NULL;
}

static void *
nouveau_exa_create_pixmap(ScreenPtr pScreen, int width, int height, int depth,
			  int usage_hint, int bitsPerPixel, int *new_pitch)
{
	ScrnInfoPtr scrn = xf86Screens[pScreen->myNum];
	NVPtr pNv = NVPTR(scrn);
	struct nouveau_pixmap *nvpix;
	int ret;

	if (!width || !height)
		return calloc(1, sizeof(*nvpix));

	if (!pNv->exa_force_cp &&
	     pNv->dev->vm_vram_size <= 32*1024*1024)
		return NULL;

	nvpix = calloc(1, sizeof(*nvpix));
	if (!nvpix)
		return NULL;

	ret = nouveau_allocate_surface(scrn, width, height, bitsPerPixel,
				       usage_hint, new_pitch, &nvpix->bo);
	if (!ret) {
		free(nvpix);
		return NULL;
	}

	return nvpix;
}

static void
nouveau_exa_destroy_pixmap(ScreenPtr pScreen, void *priv)
{
	struct nouveau_pixmap *nvpix = priv;

	if (!nvpix)
		return;

	nouveau_bo_ref(NULL, &nvpix->bo);
	free(nvpix);
}

bool
nv50_style_tiled_pixmap(PixmapPtr ppix)
{
	ScrnInfoPtr pScrn = xf86Screens[ppix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);

	return pNv->Architecture == NV_ARCH_50 &&
		(nouveau_pixmap_bo(ppix)->tile_flags &
		 NOUVEAU_BO_TILE_LAYOUT_MASK);
}

static Bool
nouveau_exa_download_from_screen(PixmapPtr pspix, int x, int y, int w, int h,
				 char *dst, int dst_pitch)
{
	ScrnInfoPtr pScrn = xf86Screens[pspix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_bo *bo;
	int src_pitch, cpp, offset;
	const char *src;
	Bool ret;

	src_pitch  = exaGetPixmapPitch(pspix);
	cpp = pspix->drawable.bitsPerPixel >> 3;
	offset = (y * src_pitch) + (x * cpp);

	if (pNv->GART) {
		if (NVAccelDownloadM2MF(pspix, x, y, w, h, dst, dst_pitch))
			return TRUE;
	}

	bo = nouveau_pixmap_bo(pspix);
	if (nouveau_bo_map(bo, NOUVEAU_BO_RD))
		return FALSE;
	src = (char *)bo->map + offset;
	ret = NVAccelMemcpyRect(dst, src, h, dst_pitch, src_pitch, w*cpp);
	nouveau_bo_unmap(bo);
	return ret;
}

static Bool
nouveau_exa_upload_to_screen(PixmapPtr pdpix, int x, int y, int w, int h,
			     char *src, int src_pitch)
{
	ScrnInfoPtr pScrn = xf86Screens[pdpix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_bo *bo;
	int dst_pitch, cpp;
	char *dst;
	Bool ret;

	dst_pitch  = exaGetPixmapPitch(pdpix);
	cpp = pdpix->drawable.bitsPerPixel >> 3;

	/* try hostdata transfer */
	if (w * h * cpp < 16*1024) /* heuristic */
	{
		if (pNv->Architecture < NV_ARCH_50) {
			if (NV04EXAUploadIFC(pScrn, src, src_pitch, pdpix,
					     x, y, w, h, cpp)) {
				exaMarkSync(pdpix->drawable.pScreen);
				return TRUE;
			}
		} else {
			if (NV50EXAUploadSIFC(src, src_pitch, pdpix,
					      x, y, w, h, cpp)) {
				exaMarkSync(pdpix->drawable.pScreen);
				return TRUE;
			}
		}
	}

	/* try gart-based transfer */
	if (pNv->GART) {
		if (NVAccelUploadM2MF(pdpix, x, y, w, h, src, src_pitch)) {
			exaMarkSync(pdpix->drawable.pScreen);
			return TRUE;
		}
	}

	/* fallback to memcpy-based transfer */
	bo = nouveau_pixmap_bo(pdpix);
	if (nouveau_bo_map(bo, NOUVEAU_BO_WR))
		return FALSE;
	dst = (char *)bo->map + (y * dst_pitch) + (x * cpp);
	ret = NVAccelMemcpyRect(dst, src, h, dst_pitch, src_pitch, w*cpp);
	nouveau_bo_unmap(bo);
	return ret;
}

Bool
nouveau_exa_pixmap_is_onscreen(PixmapPtr ppix)
{
	ScrnInfoPtr pScrn = xf86Screens[ppix->drawable.pScreen->myNum];

	if (pScrn->pScreen->GetScreenPixmap(pScrn->pScreen) == ppix)
		return TRUE;

	return FALSE;
}

Bool
nouveau_exa_init(ScreenPtr pScreen) 
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	ExaDriverPtr exa;

	exa = exaDriverAlloc();
	if (!exa) {
		pNv->NoAccel = TRUE;
		return FALSE;
	}

	exa->exa_major = EXA_VERSION_MAJOR;
	exa->exa_minor = EXA_VERSION_MINOR;
	exa->flags = EXA_OFFSCREEN_PIXMAPS;

#ifdef EXA_SUPPORTS_PREPARE_AUX
	exa->flags |= EXA_SUPPORTS_PREPARE_AUX;
#endif

	exa->PixmapIsOffscreen = nouveau_exa_pixmap_is_offscreen;
	exa->PrepareAccess = nouveau_exa_prepare_access;
	exa->FinishAccess = nouveau_exa_finish_access;

	exa->flags |= (EXA_HANDLES_PIXMAPS | EXA_MIXED_PIXMAPS);
	exa->pixmapOffsetAlign = 256;
	exa->pixmapPitchAlign = 64;

	exa->CreatePixmap2 = nouveau_exa_create_pixmap;
	exa->DestroyPixmap = nouveau_exa_destroy_pixmap;

	if (pNv->Architecture >= NV_ARCH_50) {
		exa->maxX = 8192;
		exa->maxY = 8192;
	} else
	if (pNv->Architecture >= NV_ARCH_10) {
		exa->maxX = 4096;
		exa->maxY = 4096;
	} else {
		exa->maxX = 2048;
		exa->maxY = 2048;
	}

	exa->MarkSync = nouveau_exa_mark_sync;
	exa->WaitMarker = nouveau_exa_wait_marker;

	exa->DownloadFromScreen = nouveau_exa_download_from_screen;
	exa->UploadToScreen = nouveau_exa_upload_to_screen;

	if (pNv->Architecture < NV_ARCH_50) {
		exa->PrepareCopy = NV04EXAPrepareCopy;
		exa->Copy = NV04EXACopy;
		exa->DoneCopy = NV04EXADoneCopy;

		exa->PrepareSolid = NV04EXAPrepareSolid;
		exa->Solid = NV04EXASolid;
		exa->DoneSolid = NV04EXADoneSolid;
	} else {
		exa->PrepareCopy = NV50EXAPrepareCopy;
		exa->Copy = NV50EXACopy;
		exa->DoneCopy = NV50EXADoneCopy;

		exa->PrepareSolid = NV50EXAPrepareSolid;
		exa->Solid = NV50EXASolid;
		exa->DoneSolid = NV50EXADoneSolid;
	}

	switch (pNv->Architecture) {	
	case NV_ARCH_10:
	case NV_ARCH_20:
 		exa->CheckComposite   = NV10EXACheckComposite;
 		exa->PrepareComposite = NV10EXAPrepareComposite;
 		exa->Composite        = NV10EXAComposite;
 		exa->DoneComposite    = NV10EXADoneComposite;
		break;
	case NV_ARCH_30:
		exa->CheckComposite   = NV30EXACheckComposite;
		exa->PrepareComposite = NV30EXAPrepareComposite;
		exa->Composite        = NV30EXAComposite;
		exa->DoneComposite    = NV30EXADoneComposite;
		break;
	case NV_ARCH_40:
		exa->CheckComposite   = NV40EXACheckComposite;
		exa->PrepareComposite = NV40EXAPrepareComposite;
		exa->Composite        = NV40EXAComposite;
		exa->DoneComposite    = NV40EXADoneComposite;
		break;
	case NV_ARCH_50:
		exa->CheckComposite   = NV50EXACheckComposite;
		exa->PrepareComposite = NV50EXAPrepareComposite;
		exa->Composite        = NV50EXAComposite;
		exa->DoneComposite    = NV50EXADoneComposite;
		break;
	default:
		break;
	}

	if (!exaDriverInit(pScreen, exa))
		return FALSE;

	pNv->EXADriverPtr = exa;
	return TRUE;
}
