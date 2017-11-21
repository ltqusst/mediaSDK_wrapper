/*****************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or
nondisclosure agreement with Intel Corporation and may not be copied
or disclosed except in accordance with the terms of that agreement.
Copyright(c) 2005-2014 Intel Corporation. All Rights Reserved.

*****************************************************************************/
#include "mfxvideo.h"
#include "common_utils.h"

// ATTENTION: If D3D surfaces are used, DX9_D3D or DX11_D3D must be set in project settings or hardcoded here
#ifdef WIN32
	#ifdef DX9_D3D
	#include "common_directx.h"
	#elif DX11_D3D
	#include "common_directx11.h"
	#endif
#else
	#if (MFX_VERSION_MAJOR == 1) && (MFX_VERSION_MINOR < 8)
	#include "mfxlinux.h"
	#endif
	#include "common_vaapi.h"
#endif

/* =======================================================
* Windows implementation of OS-specific utility functions
*/

mfxStatus Initialize(mfxIMPL impl, mfxVersion ver, MFXVideoSession* pSession, mfxFrameAllocator* pmfxAllocator)
{
	mfxStatus sts = MFX_ERR_NONE;
	bool bCreateSharedHandles = true;
#ifdef DX11_D3D
	impl |= MFX_IMPL_VIA_D3D11;
#endif

	// Initialize Intel Media SDK Session
	sts = pSession->Init(impl, &ver);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	// Create VA/DirectX device context
	mfxHDL deviceHandle = DeviceHandle::get(*pSession);

	// Provide device manager to Media SDK
	sts = pSession->SetHandle(DEVICE_MGR_TYPE, deviceHandle);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	// If mfxFrameAllocator is provided it means we need to setup DirectX device and memory allocator
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
#if defined(DX9_D3D) || defined(DX11_D3D)
	ClearYUVSurfaceD3D(memId);
#else
	ClearYUVSurfaceVAAPI(memId);
#endif
}

void ClearRGBSurfaceVMem(mfxMemId memId)
{
#if defined(DX9_D3D) || defined(DX11_D3D)
	ClearRGBSurfaceD3D(memId);
#else
	ClearRGBSurfaceVAAPI(memId);
#endif
}

// =================================================================
// Utility functions, not directly tied to Intel Media SDK functionality
//
std::string get_fourcc(mfxU32 fourcc)
{
	std::string name;
	for(int i=0;i<4;i++)
	{
		name.push_back(((char*)(&fourcc))[i]);
	}
	return name;
}

void PrintErrString(int err,const char* filestr,int line)
{
    switch (err) {
    case   0:
        printf("\n No error.\n");
        break;
    case  -1:
        printf("\n Unknown error: %s %d\n",filestr,line);
        break;
    case  -2:
        printf("\n Null pointer.  Check filename/path + permissions? %s %d\n",filestr,line);
        break;
    case  -3:
        printf("\n Unsupported feature/library load error. %s %d\n",filestr,line);
        break;
    case  -4:
        printf("\n Could not allocate memory. %s %d\n",filestr,line);
        break;
    case  -5:
        printf("\n Insufficient IO buffers. %s %d\n",filestr,line);
        break;
    case  -6:
        printf("\n Invalid handle. %s %d\n",filestr,line);
        break;
    case  -7:
        printf("\n Memory lock failure. %s %d\n",filestr,line);
        break;
    case  -8:
        printf("\n Function called before initialization. %s %d\n",filestr,line);
        break;
    case  -9:
        printf("\n Specified object not found. %s %d\n",filestr,line);
        break;
    case -10:
        printf("\n More input data expected. %s %d\n",filestr,line);
        break;
    case -11:
        printf("\n More output surfaces expected. %s %d\n",filestr,line);
        break;
    case -12:
        printf("\n Operation aborted. %s %d\n",filestr,line);
        break;
    case -13:
        printf("\n HW device lost. %s %d\n",filestr,line);
        break;
    case -14:
        printf("\n Incompatible video parameters. %s %d\n",filestr,line);
        break;
    case -15:
        printf("\n Invalid video parameters. %s %d\n",filestr,line);
        break;
    case -16:
        printf("\n Undefined behavior. %s %d\n",filestr,line);
        break;
    case -17:
        printf("\n Device operation failure. %s %d\n",filestr,line);
        break;
    case -18:
        printf("\n More bitstream data expected. %s %d\n",filestr,line);
        break;
    case -19:
        printf("\n Incompatible audio parameters. %s %d\n",filestr,line);
        break;
    case -20:
        printf("\n Invalid audio parameters. %s %d\n",filestr,line);
        break;
    default:
        printf("\nError code %d,\t%s\t%d\n\n", err, filestr, line);
    }
}

mfxStatus ReadPlaneData(mfxU16 w, mfxU16 h, mfxU8* buf, mfxU8* ptr,
                        mfxU16 pitch, mfxU16 offset, FILE* fSource)
{
    mfxU32 nBytesRead;
    for (mfxU16 i = 0; i < h; i++) {
        nBytesRead = (mfxU32) fread(buf, 1, w, fSource);
        if (w != nBytesRead)
            return MFX_ERR_MORE_DATA;
        for (mfxU16 j = 0; j < w; j++)
            ptr[i * pitch + j * 2 + offset] = buf[j];
    }
    return MFX_ERR_NONE;
}

mfxStatus LoadRawFrame(mfxFrameSurface1* pSurface, FILE* fSource)
{
    if (!fSource) {
        // Simulate instantaneous access to 1000 "empty" frames.
        static int frameCount = 0;
        if (1000 == frameCount++)
            return MFX_ERR_MORE_DATA;
        else
            return MFX_ERR_NONE;
    }

    mfxStatus sts = MFX_ERR_NONE;
    mfxU32 nBytesRead;
    mfxU16 w, h, i, pitch;
    mfxU8* ptr;
    mfxFrameInfo* pInfo = &pSurface->Info;
    mfxFrameData* pData = &pSurface->Data;

    if (pInfo->CropH > 0 && pInfo->CropW > 0) {
        w = pInfo->CropW;
        h = pInfo->CropH;
    } else {
        w = pInfo->Width;
        h = pInfo->Height;
    }

    pitch = pData->Pitch;
    ptr = pData->Y + pInfo->CropX + pInfo->CropY * pData->Pitch;

    // read luminance plane
    for (i = 0; i < h; i++) {
        nBytesRead = (mfxU32) fread(ptr + i * pitch, 1, w, fSource);
        if (w != nBytesRead)
            return MFX_ERR_MORE_DATA;
    }

    mfxU8 buf[2048];        // maximum supported chroma width for nv12
    w /= 2;
    h /= 2;
    ptr = pData->UV + pInfo->CropX + (pInfo->CropY / 2) * pitch;
    if (w > 2048)
        return MFX_ERR_UNSUPPORTED;

    // load U
    sts = ReadPlaneData(w, h, buf, ptr, pitch, 0, fSource);
    if (MFX_ERR_NONE != sts)
        return sts;
    // load V
    ReadPlaneData(w, h, buf, ptr, pitch, 1, fSource);
    if (MFX_ERR_NONE != sts)
        return sts;

    return MFX_ERR_NONE;
}

mfxStatus LoadRawRGBFrame(mfxFrameSurface1* pSurface, FILE* fSource)
{
    if (!fSource) {
        // Simulate instantaneous access to 1000 "empty" frames.
        static int frameCount = 0;
        if (1000 == frameCount++)
            return MFX_ERR_MORE_DATA;
        else
            return MFX_ERR_NONE;
    }

    size_t nBytesRead;
    mfxU16 w, h;
    mfxFrameInfo* pInfo = &pSurface->Info;

    if (pInfo->CropH > 0 && pInfo->CropW > 0) {
        w = pInfo->CropW;
        h = pInfo->CropH;
    } else {
        w = pInfo->Width;
        h = pInfo->Height;
    }

    for (mfxU16 i = 0; i < h; i++) {
        nBytesRead = fread(pSurface->Data.B + i * pSurface->Data.Pitch,
                           1, w * 4, fSource);
        if ((size_t)(w * 4) != nBytesRead)
            return MFX_ERR_MORE_DATA;
    }

    return MFX_ERR_NONE;
}

mfxStatus WriteBitStreamFrame(mfxBitstream* pMfxBitstream, FILE* fSink)
{
    mfxU32 nBytesWritten =
        (mfxU32) fwrite(pMfxBitstream->Data + pMfxBitstream->DataOffset, 1,
                        pMfxBitstream->DataLength, fSink);
    if (nBytesWritten != pMfxBitstream->DataLength)
        return MFX_ERR_UNDEFINED_BEHAVIOR;

    pMfxBitstream->DataLength = 0;

    return MFX_ERR_NONE;
}


mfxStatus WriteSection(mfxU8* plane, mfxU16 factor, mfxU16 chunksize,
                       mfxFrameInfo* pInfo, mfxFrameData* pData, mfxU32 i,
                       mfxU32 j, FILE* fSink)
{
    if (chunksize !=
        fwrite(plane +
               (pInfo->CropY * pData->Pitch / factor + pInfo->CropX) +
               i * pData->Pitch + j, 1, chunksize, fSink))
        return MFX_ERR_UNDEFINED_BEHAVIOR;
    return MFX_ERR_NONE;
}
mfxStatus WriteRawFrameRGB4(mfxFrameSurface1* pSurface, FILE* fSink)
{
    mfxFrameInfo* pInfo = &pSurface->Info;
    mfxFrameData* pData = &pSurface->Data;
    mfxU32 i, j, h, w;
    mfxStatus sts = MFX_ERR_NONE;
    mfxU32 pitch = ((mfxU32)pData->PitchHigh << 16) + pData->PitchLow;

    char RGB_order[5]={0};
    mfxU8 * pSrc = pData->A;
    if(pSrc > pData->R) pSrc = pData->R;
    if(pSrc > pData->G) pSrc = pData->G;
    if(pSrc > pData->B) pSrc = pData->B;

    RGB_order[(pData->R - pSrc)] = 'R';
    RGB_order[(pData->G - pSrc)] = 'G';
    RGB_order[(pData->B - pSrc)] = 'B';
    RGB_order[(pData->A - pSrc)] = 'A';

    /*
    printf("pitch=%d, order:%s crop(%d,%d,%d,%d) buff:%llu\n",
    		pitch,
    		RGB_order,
			pInfo->CropX,pInfo->CropY,pInfo->CropW, pInfo->CropH,
			pInfo->BufferSize);
*/
    for (i = 0; i < pInfo->CropH; i++)
    {
  		fwrite(pSrc + (i+pInfo->CropY) * pitch + pInfo->CropX * 4,
  				1, pInfo->CropW*4, fSink);
    }
    return sts;
}
mfxStatus WriteRawFrame(mfxFrameSurface1* pSurface, FILE* fSink)
{
    mfxFrameInfo* pInfo = &pSurface->Info;
    mfxFrameData* pData = &pSurface->Data;
    mfxU32 i, j, h, w;
    mfxStatus sts = MFX_ERR_NONE;

    if(pInfo->FourCC == MFX_FOURCC_RGB4)
    	return WriteRawFrameRGB4(pSurface, fSink);

    for (i = 0; i < pInfo->CropH; i++)
        sts =
            WriteSection(pData->Y, 1, pInfo->CropW, pInfo, pData, i, 0,
                         fSink);

    h = pInfo->CropH / 2;
    w = pInfo->CropW;
    for (i = 0; i < h; i++)
        for (j = 0; j < w; j += 2)
            sts =
                WriteSection(pData->UV, 2, 1, pInfo, pData, i, j,
                             fSink);
    for (i = 0; i < h; i++)
        for (j = 1; j < w; j += 2)
            sts =
                WriteSection(pData->UV, 2, 1, pInfo, pData, i, j,
                             fSink);

    return sts;
}

int GetFreeTaskIndex(Task* pTaskPool, mfxU16 nPoolSize)
{
    if (pTaskPool)
        for (int i = 0; i < nPoolSize; i++)
            if (!pTaskPool[i].syncp)
                return i;
    return MFX_ERR_NOT_FOUND;
}

void ClearYUVSurfaceSysMem(mfxFrameSurface1* pSfc, mfxU16 width, mfxU16 height)
{
    // In case simulating direct access to frames we initialize the allocated surfaces with default pattern
    memset(pSfc->Data.Y, 100, width * height);  // Y plane
    memset(pSfc->Data.U, 50, (width * height)/2);  // UV plane
}


// Get free raw frame surface
int GetFreeSurfaceIndex(mfxFrameSurface1** pSurfacesPool, mfxU16 nPoolSize)
{
    if (pSurfacesPool)
        for (mfxU16 i = 0; i < nPoolSize; i++)
            if (0 == pSurfacesPool[i]->Data.Locked)
                return i;
    return MFX_ERR_NOT_FOUND;
}

char mfxFrameTypeString(mfxU16 FrameType)
{
    mfxU8 FrameTmp = FrameType & 0xF;
    char FrameTypeOut;
    switch (FrameTmp) {
    case MFX_FRAMETYPE_I:
        FrameTypeOut = 'I';
        break;
    case MFX_FRAMETYPE_P:
        FrameTypeOut = 'P';
        break;
    case MFX_FRAMETYPE_B:
        FrameTypeOut = 'B';
        break;
    default:
        FrameTypeOut = '*';
    }
    return FrameTypeOut;
}

const char * mfxMemTypeString(mfxU16 memType)
{
	static char strRes[256];
	#define SHOW_FLAG(value) if(value & memType) sprintf(strRes + strlen(strRes), "[%s:0x%X] ", #value, value);
	strRes[0] = 0;
	SHOW_FLAG(MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET);
	SHOW_FLAG(MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET);
	SHOW_FLAG(MFX_MEMTYPE_SYSTEM_MEMORY);
	SHOW_FLAG(MFX_MEMTYPE_RESERVED1);

	SHOW_FLAG(MFX_MEMTYPE_FROM_ENCODE);
	SHOW_FLAG(MFX_MEMTYPE_FROM_ENCODE);
	SHOW_FLAG(MFX_MEMTYPE_FROM_DECODE);
	SHOW_FLAG(MFX_MEMTYPE_FROM_VPPIN);
	SHOW_FLAG(MFX_MEMTYPE_FROM_VPPOUT);
	SHOW_FLAG(MFX_MEMTYPE_FROM_ENC);
	SHOW_FLAG(MFX_MEMTYPE_FROM_PAK);

	SHOW_FLAG(MFX_MEMTYPE_INTERNAL_FRAME);
	SHOW_FLAG(MFX_MEMTYPE_EXTERNAL_FRAME);
	SHOW_FLAG(MFX_MEMTYPE_OPAQUE_FRAME);
	SHOW_FLAG(MFX_MEMTYPE_EXPORT_FRAME);
	SHOW_FLAG(MFX_MEMTYPE_RESERVED2);
	return strRes;
}
void mfxPrintFrameInfo(mfxFrameInfo &Info)
{
	printf("%dx%d, crop(%d,%d,%d,%d), %c%c%c%c fps(%d/%d) buff:%d chroma:0x%x",
			Info.Width, Info.Height,
			Info.CropX, Info.CropY,
			Info.CropW, Info.CropH,
			((char*)&Info.FourCC)[0],((char*)&Info.FourCC)[1],((char*)&Info.FourCC)[2],((char*)&Info.FourCC)[3],
			Info.FrameRateExtN, Info.FrameRateExtD,
			Info.BufferSize, Info.ChromaFormat
	);
}
void mfxPrintReq(mfxFrameAllocRequest *req,const char *name)
{
	printf("%s: %dx%dx(%d~%d), crop(%d,%d,%d,%d), %c%c%c%c fps(%d/%d) buff:%d chroma:0x%x\n\t%s\n", name,
			req->Info.Width, req->Info.Height,
			req->NumFrameMin, req->NumFrameSuggested,
			req->Info.CropX, req->Info.CropY,
			req->Info.CropW, req->Info.CropH,
			((char*)&req->Info.FourCC)[0],((char*)&req->Info.FourCC)[1],((char*)&req->Info.FourCC)[2],((char*)&req->Info.FourCC)[3],
			req->Info.FrameRateExtN, req->Info.FrameRateExtD,
			req->Info.BufferSize, req->Info.ChromaFormat,
			mfxMemTypeString(req->Type)
			);
}
