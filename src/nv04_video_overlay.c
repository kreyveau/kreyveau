/*
 * Copyright 2007 Arthur Huillet
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include "exa.h"
#include "damage.h"
#include "dixstruct.h"
#include "fourcc.h"

#include "nv_include.h"
#include "nv_dma.h"

extern Atom xvBrightness, xvColorKey, xvAutopaintColorKey, xvSetDefaults;

void
NV04PutOverlayImage(ScrnInfoPtr pScrn, int offset, int id,
                  int dstPitch, BoxPtr dstBox,
                  int x1, int y1, int x2, int y2,
                  short width, short height,
                  short src_w, short src_h,
                  short drw_w, short drw_h,
                  RegionPtr clipBoxes)
{                       
        NVPtr         pNv    = NVPTR(pScrn);
        NVPortPrivPtr pPriv  = GET_OVERLAY_PRIVATE(pNv);
                
        /* paint the color key */
        if(pPriv->autopaintColorKey && (pPriv->grabbedByV4L ||
                !REGION_EQUAL(pScrn->pScreen, &pPriv->clip, clipBoxes))) {
                /* we always paint V4L's color key */
                if (!pPriv->grabbedByV4L)
                        REGION_COPY(pScrn->pScreen, &pPriv->clip, clipBoxes);
                {
                xf86XVFillKeyHelper(pScrn->pScreen, pPriv->colorKey, clipBoxes);
                } 
        }         
                  
        if(pNv->CurrentLayout.mode->Flags & V_DBLSCAN) { /*This may not work with NV04 overlay according to rivatv source*/
                dstBox->y1 <<= 1;
                dstBox->y2 <<= 1;
                drw_h <<= 1; 
        }

        /* NV_PVIDEO_OE_STATE */
        /* NV_PVIDEO_SU_STATE */
        /* NV_PVIDEO_RM_STATE */
        nvWriteRAMDAC(pNv, 0, 0x224, 0);
        nvWriteRAMDAC(pNv, 0, 0x228, 0);
        nvWriteRAMDAC(pNv, 0, 0x22c, 0);

        /* NV_PVIDEO_BUFF0_START_ADDRESS */
        nvWriteRAMDAC(pNv, 0, 0x20C, offset);
        nvWriteRAMDAC(pNv, 0, 0x20C + 4, offset);
        /* NV_PVIDEO_BUFF0_PITCH_LENGTH */
        nvWriteRAMDAC(pNv, 0, 0x214, dstPitch);
        nvWriteRAMDAC(pNv, 0, 0x214 + 4, dstPitch);

        /* NV_PVIDEO_BUFF0_OFFSET */
        nvWriteRAMDAC(pNv, 0, 0x21C, 0);
        nvWriteRAMDAC(pNv, 0, 0x21C + 4, 0);

        /* NV_PVIDEO_WINDOW_START */
        nvWriteRAMDAC(pNv, 0, 0x230, (dstBox->y1 << 16) | dstBox->x1);
        /* NV_PVIDEO_WINDOW_SIZE */
        nvWriteRAMDAC(pNv, 0, 0x234, ((dstBox->y2 - dstBox->y1) << 16) |
                           (dstBox->x2 - dstBox->x1));
        /* NV_PVIDEO_STEP_SIZE */
        nvWriteRAMDAC(pNv,  0,  0x200, (uint32_t)(((src_h - 1) << 11) / (drw_h - 1)) << 16 | (uint32_t)(((src_w - 1) << 11) / (drw_w - 1)));

        /* NV_PVIDEO_RED_CSC_OFFSET */
        /* NV_PVIDEO_GREEN_CSC_OFFSET */
        /* NV_PVIDEO_BLUE_CSC_OFFSET */
        /* NV_PVIDEO_CSC_ADJUST */
        nvWriteRAMDAC(pNv, 0, 0x280, (0x69 - (pPriv->brightness * 62 / 512)));
        nvWriteRAMDAC(pNv, 0, 0x284, (0x3e + (pPriv->brightness * 62 / 512)));
        nvWriteRAMDAC(pNv, 0, 0x288, (0x89 - (pPriv->brightness * 62 / 512)));
        nvWriteRAMDAC(pNv, 0, 0x28C, 0x0);

        /* NV_PVIDEO_CONTROL_Y (BLUR_ON, LINE_HALF) */
        nvWriteRAMDAC(pNv, 0, 0x204, 0x001);
        /* NV_PVIDEO_CONTROL_X (WEIGHT_HEAVY, SHARPENING_ON, SMOOTHING_ON) */
        nvWriteRAMDAC(pNv, 0, 0x208, 0x111);

        /* NV_PVIDEO_FIFO_BURST_LENGTH */
        nvWriteRAMDAC(pNv, 0, 0x23C, 0x03);
        /* NV_PVIDEO_FIFO_THRES_SIZE */
        nvWriteRAMDAC(pNv, 0, 0x238, 0x38);

        /* Color key */
        nvWriteRAMDAC(pNv, 0, 0x240, pPriv->colorKey);

        /*NV_PVIDEO_OVERLAY
                0x1 Video on
                0x10 Use colorkey
                0x100 Format YUY2 */
        nvWriteRAMDAC(pNv, 0, 0x244, 0x111);

        /* NV_PVIDEO_SU_STATE */
        nvWriteRAMDAC(pNv, 0, 0x228, (nvReadRAMDAC(pNv, 0, 0x228) ^ (1 << 16)));

        pPriv->videoStatus = CLIENT_VIDEO_ON;
}

/**
 * NV04SetOverlayPortAttribute
 * sets the attribute "attribute" of port "data" to value "value"
 * calls NVResetVideo(pScrn) to apply changes to hardware
 *                      
 * @param pScrenInfo
 * @param attribute attribute to set
 * @param value value to which attribute is to be set
 * @param data port from which the attribute is to be set
 * 
 * @return Success, if setting is successful
 * BadValue/BadMatch, if value/attribute are invalid
 * @see NVResetVideo(ScrnInfoPtr pScrn)
 */
int
NV04SetOverlayPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
                          INT32 value, pointer data)
{
        NVPortPrivPtr pPriv = (NVPortPrivPtr)data;
        NVPtr         pNv   = NVPTR(pScrn);

        if (attribute == xvBrightness) {
                if ((value < -512) || (value > 512))
                        return BadValue;
                pPriv->brightness = value;
        } else
        if (attribute == xvColorKey) {
                pPriv->colorKey = value;
                REGION_EMPTY(pScrn->pScreen, &pPriv->clip);
        } else
        if (attribute == xvAutopaintColorKey) {
                if ((value < 0) || (value > 1))
                        return BadValue;
                pPriv->autopaintColorKey = value;
        } else
        if (attribute == xvSetDefaults) {
                NVSetPortDefaults(pScrn, pPriv);
        } else
                return BadMatch;

        return Success;
}

/**
 * NV04GetOverlayPortAttribute
 * 
 * @param pScrn unused
 * @param attribute attribute to be read
 * @param value value of attribute will be stored in this pointer
 * @param data port from which attribute will be read
 * @return Success, if queried attribute exists
 */
int
NV04GetOverlayPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
                          INT32 *value, pointer data)
{
        NVPortPrivPtr pPriv = (NVPortPrivPtr)data;

        if (attribute == xvBrightness)
                *value = pPriv->brightness;
        else if (attribute == xvColorKey)
                *value = pPriv->colorKey;
        else if (attribute == xvAutopaintColorKey)
                *value = (pPriv->autopaintColorKey) ? 1 : 0;
        else
                return BadMatch;

        return Success;
}

/**
 * NV04StopOverlay
 * Tell the hardware to stop the overlay
 */
void
NV04StopOverlay (ScrnInfoPtr pScrn)
{
    NVPtr pNv = NVPTR(pScrn);

    nvWriteRAMDAC(pNv, 0, 0x244, nvReadRAMDAC(pNv, 0, 0x244) &~ 0x1);
    nvWriteRAMDAC(pNv, 0, 0x224, 0);
    nvWriteRAMDAC(pNv, 0, 0x228, 0);
    nvWriteRAMDAC(pNv, 0, 0x22c, 0);
}

