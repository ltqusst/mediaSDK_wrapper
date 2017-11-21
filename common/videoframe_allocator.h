#ifndef VIDEOFRAME_ALLOCATOR_H
#define VIDEOFRAME_ALLOCATOR_H

#include "mfxvideo.h"
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string.h>

#include "common_utils.h"
// =================================================================
// Intel Media SDK memory allocator entrypoints....
// Implementation of this functions is OS/Memory type specific.

class mem_allocator
{
public:
	virtual ~mem_allocator(){}
	virtual bool is_mytype(int type)=0;
	virtual mfxStatus do_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)=0;
	virtual mfxStatus do_lock(mfxMemId mid, mfxFrameData* ptr)=0;
	virtual mfxStatus do_unlock(mfxMemId mid, mfxFrameData* ptr)=0;
	virtual mfxStatus do_gethdl(mfxMemId mid, mfxHDL* handle)=0;
	virtual mfxStatus do_free(mfxFrameAllocResponse* response)=0;
};
class mem_allocator_system:public mem_allocator
{
public:
	virtual ~mem_allocator_system(){}
	virtual bool is_mytype(int type){return ((type & MFX_MEMTYPE_SYSTEM_MEMORY) == MFX_MEMTYPE_SYSTEM_MEMORY);}
	virtual mfxStatus do_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
	virtual mfxStatus do_lock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus do_unlock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus do_gethdl(mfxMemId mid, mfxHDL* handle);
	virtual mfxStatus do_free(mfxFrameAllocResponse* response);
};
// Win32/Linux platform dependent implementations are required for this allocator to work
class mem_allocator_video:public mem_allocator
{
public:
	virtual ~mem_allocator_video(){}
	virtual bool is_mytype(int type){return ((type & MFX_MEMTYPE_SYSTEM_MEMORY) == 0);}
	virtual mfxStatus do_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
	virtual mfxStatus do_lock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus do_unlock(mfxMemId mid, mfxFrameData* ptr);
	virtual mfxStatus do_gethdl(mfxMemId mid, mfxHDL* handle);
	virtual mfxStatus do_free(mfxFrameAllocResponse* response);
};


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

		m_allocators.push_back(std::shared_ptr<mem_allocator>(new mem_allocator_system()));
		m_allocators.push_back(std::shared_ptr<mem_allocator>(new mem_allocator_video()));
	}

    mfxFrameAllocResponse 			m_mfxResponse;
    int 							m_refCount;

	std::set<mfxMemId*>     		m_SetMemId;

	int 							m_alloc_count;
	int 							m_free_count;

	std::vector<std::shared_ptr<mem_allocator>>	    m_allocators;

	static mfxStatus _alloc(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
	static mfxStatus _lock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
	static mfxStatus _unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
	static mfxStatus _gethdl(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);
	static mfxStatus _free(mfxHDL pthis, mfxFrameAllocResponse* response);
};


#endif

