#include "mfxvideo.h"
#include <set>
#include <string.h>
#include <algorithm>

#include "videoframe_allocator.h"

mfxStatus videoframe_allocator::simple_alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
    mfxStatus sts = MFX_ERR_NONE;

    if (0 == request || 0 == response || 0 == request->NumFrameSuggested)
        return MFX_ERR_MEMORY_ALLOC;

    if ((request->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET |
                          MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET)) == 0)
        return MFX_ERR_UNSUPPORTED;

    //there may be some redundant request!
    bool b_possible_redundant_req =
    		MFX_MEMTYPE_EXTERNAL_FRAME & request->Type
            && MFX_MEMTYPE_FROM_DECODE & request->Type;

    if(b_possible_redundant_req && m_mfxResponse.NumFrameActual > 0)
    {
        // Memory for this request was already allocated during manual allocation stage. Return saved response
        //   When decode acceleration device (VAAPI) is created it requires a list of VAAPI surfaces to be passed.
        //   Therefore Media SDK will ask for the surface info/mids again at Init() stage, thus requiring us to return the saved response
        //   (No such restriction applies to Encode or VPP)
        if (request->AllocId == m_mfxResponse.AllocId
        	&& request->NumFrameSuggested <= m_mfxResponse.NumFrameActual)
        {
            *response = m_mfxResponse;
            m_refCount++;
            return MFX_ERR_NONE;
        }
    }

	sts = ::do_alloc(request, response);

	if (MFX_ERR_NONE == sts) {
		if (b_possible_redundant_req) {
			// Decode alloc response handling
			m_mfxResponse = *response;
			m_refCount++;
		} else {
			// Encode and VPP alloc response handling
			m_SetMemId.insert(response->mids);
		}
	}

    return sts;
}

mfxStatus videoframe_allocator::simple_free(mfxFrameAllocResponse* response)
{
    if (!response) return MFX_ERR_NULL_PTR;

    if(response->mids == m_mfxResponse.mids){
    	//redundant response
    	if(--m_refCount == 0){
    		::do_free(response);
    		memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
    	}
    	//success even when no actual free is needed
    	return MFX_ERR_NONE;
    }

    std::set<mfxMemId*>::iterator it = m_SetMemId.find(response->mids);

    if(it !=  m_SetMemId.end()){
    	::do_free(response);
    	m_SetMemId.erase(it);
    	return MFX_ERR_NONE;
    }

    return MFX_ERR_NOT_FOUND;
}
mfxStatus videoframe_allocator::simple_lock(mfxMemId mid, mfxFrameData* ptr)
{
	return ::do_lock(mid,ptr);
}
mfxStatus videoframe_allocator::simple_unlock(mfxMemId mid, mfxFrameData* ptr)
{
	return ::do_unlock(mid,ptr);
}
mfxStatus videoframe_allocator::simple_gethdl(mfxMemId mid, mfxHDL* handle)
{
	return ::do_gethdl(mid, handle);
}
