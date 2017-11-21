#include "mfxvideo.h"
#include <set>
#include <string.h>
#include <algorithm>

#include "videoframe_allocator.h"
#include "cassert"

//===================================================================
struct sysmem_frame : public mfxFrameData
{
	mfxU32  FourCC;
	mfxU8 * pBuffer;
	sysmem_frame(const mfxFrameInfo & info){
		memset((mfxFrameData*)this,0, sizeof(mfxFrameData));
		FourCC = info.FourCC;
		pBuffer = NULL;

		switch(FourCC){
		case MFX_FOURCC_NV12:
			this->Pitch = info.Width;
			pBuffer = (mfxU8*)malloc(this->Pitch * info.Height + (this->Pitch * info.Height/2));
			this->Y = pBuffer;
			this->U = this->Y + this->Pitch * info.Height;
			this->V = this->U + 1;
			break;
		case MFX_FOURCC_RGB4:
			this->Pitch = info.Width * 4;
			pBuffer = (mfxU8*)malloc(this->Pitch * info.Height);
			this->B = pBuffer;
			this->G = this->B + 1;
			this->R = this->B + 2;
			this->A = this->B + 3;
			break;
		default:
			this->Pitch = 0;
			break;
		}
	}
	~sysmem_frame(){
		if(pBuffer) free(pBuffer);
	}
};
//===================================================================
mfxStatus mem_allocator_system::do_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
	if((request->Type & MFX_MEMTYPE_SYSTEM_MEMORY) != MFX_MEMTYPE_SYSTEM_MEMORY)
		return MFX_ERR_UNSUPPORTED;

	int N = request->NumFrameSuggested;
	response->mids = (mfxMemId*)calloc(N, sizeof(mfxMemId));

	for(int i=0; i<N; i++)
		response->mids[i] = (mfxMemId)new sysmem_frame(request->Info);

	response->NumFrameActual = N;
	return MFX_ERR_NONE;
}
mfxStatus mem_allocator_system::do_free(mfxFrameAllocResponse* response)
{
	if(response->NumFrameActual <= 0) return MFX_ERR_NONE;

	for(int i=0; i<response->NumFrameActual; i++)
		delete response->mids[i];

	free(response->mids);
	return MFX_ERR_NONE;
}
mfxStatus mem_allocator_system::do_lock(mfxMemId mid, mfxFrameData* ptr)
{
	sysmem_frame * p = (sysmem_frame*) mid;
	ptr->R = p->R;
	ptr->G = p->G;
	ptr->B = p->B;
	ptr->A = p->A;
	ptr->Pitch = p->Pitch;
	ptr->PitchHigh = p->PitchHigh;
	return MFX_ERR_NONE;
}
mfxStatus mem_allocator_system::do_unlock(mfxMemId mid, mfxFrameData* ptr)
{
	mid;
	ptr;
	return MFX_ERR_NONE;
}
mfxStatus mem_allocator_system::do_gethdl(mfxMemId mid, mfxHDL* handle)
{
	*handle = mid;
	printf("????????????????????????????????\n");
	return MFX_ERR_NONE;
}


//===================================================================
//each mfxMemId in mfxFrameAllocResponse do not contain type id
//so its hard to distribute
struct typped_memid
{
	mem_allocator*  allocator;	// which internal allocator should be responsible
	mfxMemId  		mid;

	//helper functions
	static void wrap(mfxFrameAllocResponse* response, mem_allocator * allocator);
	static mem_allocator * unwrap(mfxFrameAllocResponse* response);
};

void typped_memid::wrap(mfxFrameAllocResponse* response, mem_allocator * allocator){
	//replace each item
	for(int i=0; i<response->NumFrameActual; i++){
		typped_memid * pt = new typped_memid();
		pt->allocator = allocator;
		pt->mid = response->mids[i];
		response->mids[i] = pt;
	}
}
mem_allocator * typped_memid::unwrap(mfxFrameAllocResponse* response){
	mem_allocator * allocator;
	for(int i=0; i<response->NumFrameActual; i++){
		typped_memid * pt = (typped_memid *) response->mids[i];
		allocator = pt->allocator;
		response->mids[i] = pt->mid;
		delete pt;
	}
	return allocator;
}

//===================================================================
mfxStatus videoframe_allocator::_alloc(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
	videoframe_allocator * that = (videoframe_allocator*)pthis;
    mfxStatus sts = MFX_ERR_NONE;
    //there may be some redundant request!
    bool b_possible_redundant_req =   MFX_MEMTYPE_EXTERNAL_FRAME & request->Type
                                   && MFX_MEMTYPE_FROM_DECODE & request->Type;
    int allocator_id = -1;

    if (0 == request || 0 == response || 0 == request->NumFrameSuggested){
        sts = MFX_ERR_MEMORY_ALLOC;
        goto Exit;
    }

    if ((request->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET |
                          MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET |
						  MFX_MEMTYPE_SYSTEM_MEMORY)) == 0){
        sts = MFX_ERR_UNSUPPORTED;
        goto Exit;
    }

    if(b_possible_redundant_req && that->m_mfxResponse.NumFrameActual > 0)
    {
        // Memory for this request was already allocated during manual allocation stage. Return saved response
        //   When decode acceleration device (VAAPI) is created it requires a list of VAAPI surfaces to be passed.
        //   Therefore Media SDK will ask for the surface info/mids again at Init() stage, thus requiring us to return the saved response
        //   (No such restriction applies to Encode or VPP)
        if (request->AllocId == that->m_mfxResponse.AllocId
        	&& request->NumFrameSuggested <= that->m_mfxResponse.NumFrameActual)
        {
            *response = that->m_mfxResponse;
            that->m_refCount++;
            sts = MFX_ERR_NONE;
            goto Exit;
        }
    }

    //type based distribution
    for(allocator_id=0; allocator_id < that->m_allocators.size(); allocator_id++){
    	auto &a = that->m_allocators[allocator_id];
		if(a->is_mytype(request->Type)){
			sts = a->do_alloc(request, response);
			break;
		}
	}

    if(allocator_id >= that->m_allocators.size()){
    	sts = MFX_ERR_UNSUPPORTED;
    	assert(0);
    	goto Exit;
    }

	if (MFX_ERR_NONE == sts) {
		response->AllocId = request->AllocId;

		typped_memid::wrap(response, that->m_allocators[allocator_id].get());

		if (b_possible_redundant_req) {
			// Decode alloc response handling
			that->m_mfxResponse = *response;
			that->m_refCount++;
		} else {
			// Encode and VPP alloc response handling
			that->m_SetMemId.insert(response->mids);
		}
	}

	that->m_alloc_count ++;

	PDEBUG( ANSI_COLOR_YELLOW "_alloc(%p)@%d id:%d type:0x%x %dx%dx(%d~%d) response  mids:%p cnt:%u sts:%d\n" ANSI_COLOR_RESET,
			pthis, that->m_alloc_count, request->AllocId,
			request->Type, request->Info.Width, request->Info.Height, request->NumFrameMin, request->NumFrameSuggested,
			response->mids, response->NumFrameActual, sts);
Exit:
	if(sts != MFX_ERR_NONE)
		fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "%s:%d simple_alloc() failed %d!\n" ANSI_COLOR_RESET,__FILE__,__LINE__, sts);

	return sts;
}
mfxStatus videoframe_allocator::_free(mfxHDL pthis, mfxFrameAllocResponse* response){
	videoframe_allocator * that = (videoframe_allocator*)pthis;
	mfxStatus sts;
	std::set<mfxMemId*>::iterator it;

    if (!response) {
    	sts = MFX_ERR_NULL_PTR;
    	goto Exit;
    }

    if(response->mids == that->m_mfxResponse.mids){
    	//redundant response
    	if(--that->m_refCount == 0){

    		//replace each item back
    		mem_allocator * palloc = typped_memid::unwrap(response);
    		palloc->do_free(response);

    		memset(&that->m_mfxResponse, 0, sizeof(that->m_mfxResponse));
    	}
    	//success even when no actual free is needed
    	sts = MFX_ERR_NONE;
    	goto Exit;
    }

    it = that->m_SetMemId.find(response->mids);
    if(it !=  that->m_SetMemId.end()){
		//replace each item back
    	mem_allocator * palloc = typped_memid::unwrap(response);
    	palloc->do_free(response);

		that->m_SetMemId.erase(it);
    	sts = MFX_ERR_NONE;
    	goto Exit;
    }

    sts = MFX_ERR_NOT_FOUND;

	that->m_free_count ++;

	PDEBUG( ANSI_COLOR_YELLOW "_free(%p)@%d mid:%p cnt:%u sts:%d\n" ANSI_COLOR_RESET,
			pthis, that->m_free_count,
			response->mids, response->NumFrameActual, sts);
Exit:
	if(sts != MFX_ERR_NONE)
		fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "%s:%d simple_free() failed %d!\n" ANSI_COLOR_RESET,__FILE__,__LINE__, sts);

	return sts;
}

mfxStatus videoframe_allocator::_lock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr){
	typped_memid * pt = (typped_memid *) mid;
	mfxStatus sts = pt->allocator->do_lock(pt->mid,ptr);
	PDEBUG( ANSI_COLOR_YELLOW "_lock(%p) mid:%p ptr:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, ptr, sts);
	return sts;
}
mfxStatus videoframe_allocator::_unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr){
	typped_memid * pt = (typped_memid *) mid;
	mfxStatus sts = pt->allocator->do_unlock(pt->mid,ptr);
	PDEBUG( ANSI_COLOR_YELLOW "_unlock(%p) mid:%p ptr:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, ptr, sts);
	return sts;
}
mfxStatus videoframe_allocator::_gethdl(mfxHDL pthis, mfxMemId mid, mfxHDL* handle){
	typped_memid * pt = (typped_memid *) mid;
	mfxStatus sts = pt->allocator->do_gethdl(pt->mid,handle);
	PDEBUG( ANSI_COLOR_YELLOW "_gethdl(%p): mid:%p handle:%p sts:%d\n" ANSI_COLOR_RESET, pthis, mid, *handle, sts);
	return sts;
}

