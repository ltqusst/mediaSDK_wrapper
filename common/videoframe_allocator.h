#ifndef VIDEOFRAME_ALLOCATOR_H
#define VIDEOFRAME_ALLOCATOR_H

#include "mfxvideo.h"
#include <map>
#include <set>
#include <string.h>

#include "common_utils.h"
// =================================================================
// Intel Media SDK memory allocator entrypoints....
// Implementation of this functions is OS/Memory type specific.

//#define PDEBUG(s, ...) printf(s, __VA_ARGS__)
#define PDEBUG(s, ...)

class videoframe_allocator: public mfxFrameAllocator
{
public:
	videoframe_allocator()
	{
		mfxFrameAllocator::pthis = this;
		mfxFrameAllocator::Alloc  = _alloc;
		mfxFrameAllocator::Free   = _free;
		mfxFrameAllocator::Lock   = _lock;
		mfxFrameAllocator::Unlock = _unlock;
		mfxFrameAllocator::GetHDL = _gethdl;

		memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
		m_refCount = 0;

		m_alloc_count = 0;
		m_free_count = 0;
	}

    mfxFrameAllocResponse 			m_mfxResponse;
    int 							m_refCount;

	std::set<mfxMemId*>     		m_SetMemId;

	int 							m_alloc_count;
	int 							m_free_count;

	static mfxStatus _alloc(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)	{
		videoframe_allocator * that = (videoframe_allocator*)pthis;

		mfxStatus sts = that->simple_alloc(request, response);
		that->m_alloc_count ++;

		PDEBUG( ANSI_COLOR_YELLOW "_alloc(%p)@%d id:%d type:0x%x %dx%dx(%d~%d) response  mids:%p cnt:%u sts:%d\n" ANSI_COLOR_RESET,
				pthis, that->m_alloc_count, request->AllocId,
				request->Type, request->Info.Width, request->Info.Height, request->NumFrameMin, request->NumFrameSuggested,
				response->mids, response->NumFrameActual, sts);
		if(sts != MFX_ERR_NONE)
			fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "%s:%d simple_alloc() failed %d!\n" ANSI_COLOR_RESET,__FILE__,__LINE__, sts);
		return sts;
	}
	static mfxStatus _lock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr){
		videoframe_allocator * that = (videoframe_allocator*)pthis;
		mfxStatus sts =  that->simple_lock(mid, ptr);
		PDEBUG( ANSI_COLOR_YELLOW "_lock(%p) mid:%p ptr:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, ptr, sts);
		return sts;
	}
	static mfxStatus _unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr){
		mfxStatus sts =  ((videoframe_allocator*)pthis)->simple_unlock(mid, ptr);
		PDEBUG( ANSI_COLOR_YELLOW "_unlock(%p) mid:%p ptr:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, ptr, sts);
		return sts;
	}
	static mfxStatus _gethdl(mfxHDL pthis, mfxMemId mid, mfxHDL* handle){
		mfxStatus sts =  ((videoframe_allocator*)pthis)->simple_gethdl(mid, handle);
		//PDEBUG( ANSI_COLOR_YELLOW "_gethdl(%p): mid:%p handle:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, handle, sts);
		return sts;
	}
	static mfxStatus _free(mfxHDL pthis, mfxFrameAllocResponse* response){
		videoframe_allocator * that = (videoframe_allocator*)pthis;
		mfxStatus sts = ((videoframe_allocator*)pthis)->simple_free(response);
		that->m_free_count ++;

		PDEBUG( ANSI_COLOR_YELLOW "_free(%p)@%d mid:%p cnt:%u sts:%d\n" ANSI_COLOR_RESET,
				pthis, that->m_free_count,
				response->mids, response->NumFrameActual, sts);

		if(sts != MFX_ERR_NONE)
			fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "%s:%d simple_free() failed %d!\n" ANSI_COLOR_RESET,__FILE__,__LINE__, sts);

		return sts;
	}

	virtual mfxStatus simple_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
	virtual mfxStatus simple_lock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus simple_unlock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus simple_gethdl(mfxMemId mid, mfxHDL* handle);
	virtual mfxStatus simple_free(mfxFrameAllocResponse* response);
};

// Win32/Linux platform dependent implementations are required for this allocator to work

mfxStatus do_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
mfxStatus do_free(mfxFrameAllocResponse* response);
mfxStatus do_lock(mfxMemId mid, mfxFrameData* ptr);
mfxStatus do_unlock(mfxMemId mid, mfxFrameData* ptr);
mfxStatus do_gethdl(mfxMemId mid, mfxHDL* handle);

#endif

