/*****************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or
nondisclosure agreement with Intel Corporation and may not be copied
or disclosed except in accordance with the terms of that agreement.
Copyright(c) 2005-2014 Intel Corporation. All Rights Reserved.

*****************************************************************************/

#include "mfxvideo.h"
#if (MFX_VERSION_MAJOR == 1) && (MFX_VERSION_MINOR < 8)
#include "mfxlinux.h"
#endif

#include "common_utils.h"
#include "common_vaapi.h"


/* =====================================================
 * Linux implementation of OS-specific utility functions
 */

mfxStatus Initialize(mfxIMPL impl, mfxVersion ver, MFXVideoSession* pSession, mfxFrameAllocator* pmfxAllocator)
{
    mfxStatus sts = MFX_ERR_NONE;

    // Initialize Intel Media SDK Session
    sts = pSession->Init(impl, &ver);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    mfxHDL displayHandle = VAHandle::get();

    // Provide VA display handle to Media SDK
    sts = pSession->SetHandle(static_cast < mfxHandleType >(MFX_HANDLE_VA_DISPLAY), displayHandle);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // If mfxFrameAllocator is provided it means we need to setup  memory allocator
    if (pmfxAllocator) {
        // Since we are using video memory we must provide Media SDK with an external allocator
        sts = pSession->SetFrameAllocator(pmfxAllocator);
        MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
    }

    return sts;
}

void Release()
{
}

void ClearYUVSurfaceVMem(mfxMemId memId)
{
    ClearYUVSurfaceVAAPI(memId);
}

void ClearRGBSurfaceVMem(mfxMemId memId)
{
    ClearRGBSurfaceVAAPI(memId);
}
