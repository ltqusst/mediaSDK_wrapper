#ifndef _SURFACE_POOL_H_
#define _SURFACE_POOL_H_

#include <vector>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <thread>
#include <memory>
#include <mutex>
#include <functional>
#include <deque>
#include <condition_variable>

#include <string.h>

#include "mfxvideo.h"

class surface_pool;

class surface1: public mfxFrameSurface1{
public:
	surface1(const mfxFrameAllocator & mfxAllocator,
                 const mfxFrameInfo * pfmt = NULL):
                	 m_bReserved(false),
					 m_bLockedByAllocator(false),
					 m_mfxAllocator(mfxAllocator)
	{
		memset((mfxFrameSurface1*)this, 0, sizeof(mfxFrameSurface1));
		if(pfmt)
			memcpy(&(this->mfxFrameSurface1::Info), pfmt, sizeof(mfxFrameInfo));
	}
	~surface1(){ unlock();}

	//have to manually write copy-constructor because of atomic member
	surface1(const surface1 & rhs):
		mfxFrameSurface1(rhs),
		m_mfxAllocator(rhs.m_mfxAllocator)
	{
		m_bReserved.store(rhs.m_bReserved.load());
		m_bLockedByAllocator = rhs.m_bLockedByAllocator;
		m_FrameNumber = rhs.m_FrameNumber;
	}

	//after successfully Lock(), the Pitch,YUV,RGB field of Data member will be set
	mfxStatus lock()
	{
		mfxStatus sts = m_mfxAllocator.Lock(m_mfxAllocator.pthis, this->Data.MemId, &this->Data);
		if(sts == MFX_ERR_NONE)
			m_bLockedByAllocator = (true);
		return sts;
	}

	//after successfully Unlock(), the Pitch,YUV,RGB field of Data member will be 0/NULL
	mfxStatus unlock()
	{
		if(m_bLockedByAllocator == false) return MFX_ERR_NONE;

		mfxStatus sts = m_mfxAllocator.Unlock(m_mfxAllocator.pthis, this->Data.MemId, &this->Data);
		if(sts == MFX_ERR_NONE)
			m_bLockedByAllocator = (false);
		return sts;
	}

	unsigned long m_FrameNumber = 0;

private:

	// reserve operation is only allowed by pool object
	// and since the pool only reserves in one decoding thread
	// no need to use atomic
	void reserve(bool bset)	{ m_bReserved.store(bset); }
	bool is_reserved(void)	{ return m_bReserved.load(); }

	std::atomic<bool> m_bReserved;
	bool m_bLockedByAllocator;
	const mfxFrameAllocator & m_mfxAllocator;

	friend class surface_pool;
};


class surface_pool
{
public:
	surface_pool(const mfxFrameAllocator & mfxAllocator);
	~surface_pool();

	mfxStatus realloc(mfxFrameAllocRequest Request, int cntReserved = 0);

	// Get free raw frame surface
	surface1 * getfree(bool bVerbose = false);

	int surfaceID(surface1 * psurf);

	surface1 * find(mfxU32 FrameOrder);

	//Reserved surface will not be allocated again
	bool reserve(surface1 * psurf);
	bool unreserve(surface1 * psurf);


	void debug(void);

private:
	void _clear();

	mfxFrameAllocResponse   		m_mfxResponse;
	const mfxFrameAllocator &      	m_mfxAllocator;

	std::vector<surface1> 		m_SurfaceQueue;
	std::mutex            			m_SurfaceQMutex;
	int                             m_ReservedMaxCnt;
	int                             m_ReservedCnt;
	std::condition_variable     	m_cvReserve;
};

#endif

