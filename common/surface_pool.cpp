

#include "surface_pool.h"

surface_pool::surface_pool(const mfxFrameAllocator & mfxAllocator):
		m_mfxAllocator(mfxAllocator)
{
	memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
	_clear();
}

surface_pool::~surface_pool()
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	_clear();
}

mfxStatus surface_pool::realloc(mfxFrameAllocRequest Request, int cntReserved)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);

	mfxFrameAllocResponse mfxResponse;

	Request.NumFrameMin += cntReserved;
	Request.NumFrameSuggested += cntReserved;

	mfxStatus sts = m_mfxAllocator.Alloc(m_mfxAllocator.pthis, &Request, &mfxResponse);
	if(MFX_ERR_NONE != sts) return sts;

	//clear old pool only when new allocation success
	_clear();

	//replace configuration data
	m_ReservedMaxCnt = cntReserved;
	m_mfxResponse = mfxResponse;

	// Allocate surface headers (mfxFrameSurface1) for decoder
	for (int i = 0; i < m_mfxResponse.NumFrameActual; i++) {
		surface1 s(m_mfxAllocator, &(Request.Info));
		s.Data.MemId = m_mfxResponse.mids[i];
		m_SurfaceQueue.push_back(s);
	}

	return MFX_ERR_NONE;
}

// Get free raw frame surface
surface1 * surface_pool::getfree(bool bVerbose)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);

	for (int i = 0; i < m_SurfaceQueue.size(); i++)
		if (0 == m_SurfaceQueue[i].Data.Locked && !m_SurfaceQueue[i].is_reserved()){
			if(bVerbose) printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Locking: %d\n", i);
			return &(m_SurfaceQueue[i]);
		}
	return NULL;
}

int surface_pool::surfaceID(surface1 * psurf)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	for (int i = 0; i < m_SurfaceQueue.size(); i++)
		if(psurf == &(m_SurfaceQueue[i]))
			return i;
	return -1;
}

surface1 * surface_pool::find(mfxU32 FrameOrder)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	for(auto &s: m_SurfaceQueue)
		if(s.Data.FrameOrder == FrameOrder)
			return &s;
	return NULL;
}

//Reserved surface will not be allocated again
bool surface_pool::reserve(surface1 * psurf)
{
	std::unique_lock<std::mutex> guard(m_SurfaceQMutex);
	// reserved surface is for application thread to consume
	// the output surface in an async-way. also number of reserved surfaces
	// is hard limited so decoding is not affected by the reservation.
	for(auto &s: m_SurfaceQueue){
		if(psurf == &s){
			//multiple reserve is allowed
			if(s.is_reserved()) return true;

			//we cannot fail here, wait until more surface available
			m_cvReserve.wait(guard, [this](){return m_ReservedCnt < m_ReservedMaxCnt;});

			if(m_ReservedCnt < m_ReservedMaxCnt){
				s.reserve(true);
				m_ReservedCnt ++;
				return true;
			}else{
				printf("m_ReservedCnt =%d, %d, Reserve failed!\n", m_ReservedCnt, m_ReservedMaxCnt);
			}
			return false;
		}
	}
	return false;
}

bool surface_pool::unreserve(surface1 * psurf)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);

	for(auto &s: m_SurfaceQueue){
		if(psurf == &s){
			//multiple reserve is allowed
			if(!s.is_reserved())
				return true; //already unreserved

			s.reserve(false);
			m_ReservedCnt --;
			m_cvReserve.notify_all();
			return true;
		}
	}
	return false; //not found
}

//
void surface_pool::debug(void)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Locked:");
	for (int i = 0; i < m_SurfaceQueue.size(); i++)
	{
		if(m_SurfaceQueue[i].Data.Locked)
			printf("%s%d,", m_SurfaceQueue[i].is_reserved()?"R":"", i);
	}

	printf(" Free: ");
	for (int i = 0; i < m_SurfaceQueue.size(); i++){
		if(!m_SurfaceQueue[i].Data.Locked && !m_SurfaceQueue[i].is_reserved())
			printf("%d,", i);
	}

	printf(" Reserved Free: ");
	for (int i = 0; i < m_SurfaceQueue.size(); i++){
		if(!m_SurfaceQueue[i].Data.Locked && m_SurfaceQueue[i].is_reserved())
			printf("%d,", i);
	}
	printf("\n");
}


void surface_pool::_clear()
{
	m_ReservedCnt = 0;
	m_SurfaceQueue.clear();

	if(m_mfxResponse.NumFrameActual > 0)
		m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);
	memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
}
