
#include "surface_pool.h"
#include "common_utils.h"

//#define PDEBUG(...) printf(__VA_ARGS__)
#define PDEBUG(...)

#ifdef NDEBUG
#define CONSISTENCY_CHECK 0
#else
#define CONSISTENCY_CHECK 1
#endif


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
		surface1 s(m_mfxAllocator, &(Request.Info), i);
		s.Data.MemId = m_mfxResponse.mids[i];
		m_SurfaceAll.push_back(s);
	}

	return MFX_ERR_NONE;
}

// Get free raw frame surface
surface1 * surface_pool::getfree(void)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	for (auto &s : m_SurfaceAll) {
		if (s.Data.Locked == 0 && !s.is_reserved()) {
			//PDEBUG(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> getfree() Locking %d\n", s.index());
			return &s;
		}
	}
	PDEBUG(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> getfree() failed Locking\n");
	return NULL;
}

int surface_pool::surfaceID(surface1 * psurf)
{
	return psurf->index();
}

surface1 * surface_pool::find(mfxU32 FrameOrder)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	for(auto &s: m_SurfaceAll)
		if(s.Data.FrameOrder == FrameOrder)
			return &s;
	return NULL;
}

surface1 * surface_pool::find(surface1 * psurf)
{
	int i = psurf->index();
	if(i >=0 && i < m_SurfaceAll.size() && psurf == &m_SurfaceAll[i])
		return psurf;

	return NULL;
}

//Reserved surface will not be allocated again
bool surface_pool::reserve(surface1 * psurf, bool drop_on_overflow)
{
#if CONSISTENCY_CHECK 
	assert(find(psurf) != NULL || (printf("%s:%d invalid input parameter\n",__FILE__, __LINE__), 0));
#endif

	//multiple reserve is acceptable(w/o cause count increase)
	if (psurf->is_reserved()) 
		return true;

	std::unique_lock<std::mutex> guard(m_SurfaceQMutex);
	
	if (!drop_on_overflow) {
		m_cvReserve.wait(guard, [this]() {return m_ReservedCnt < m_ReservedMaxCnt; });
		psurf->reserve(true);
		m_ReservedCnt++;
		return true;
	}
	else if (m_ReservedCnt < m_ReservedMaxCnt) {
		psurf->reserve(true);
		m_ReservedCnt++;
		return true;
	}

	PDEBUG("  reserve() failed because of overflow \n");
	return false;
}

bool surface_pool::unreserve(surface1 * psurf)
{
#if CONSISTENCY_CHECK 
	assert(find(psurf) != NULL || (printf("%s:%d invalid input parameter\n", __FILE__, __LINE__), 0));
#endif

	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	
	if (psurf->is_reserved()) {
		psurf->reserve(false);
		m_ReservedCnt--;
		m_cvReserve.notify_all();
	}
	return true;
}

//
void surface_pool::debug(void)
{
	std::lock_guard<std::mutex> guard(m_SurfaceQMutex);
	bool bIndexConsistent = true;
	for (int i = 0; i < m_SurfaceAll.size(); i++)
		if(i != m_SurfaceAll[i].index()) bIndexConsistent = false;

	if(!bIndexConsistent)
		printf(ANSI_BOLD ANSI_COLOR_RED " index in consistency found! \n" ANSI_COLOR_RESET);

	printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Locked:");
	for (int i = 0; i < m_SurfaceAll.size(); i++)
	{
		if(m_SurfaceAll[i].Data.Locked)
			printf("%s%d,", m_SurfaceAll[i].is_reserved()?"R":"", i);
	}

	printf(" Free: ");
	for (int i = 0; i < m_SurfaceAll.size(); i++){
		if(!m_SurfaceAll[i].Data.Locked && !m_SurfaceAll[i].is_reserved())
			printf("%d,", i);
	}

	printf(" Reserved Free: ");
	for (int i = 0; i < m_SurfaceAll.size(); i++){
		if(!m_SurfaceAll[i].Data.Locked && m_SurfaceAll[i].is_reserved())
			printf("%d,", i);
	}
	printf("\n");
}


void surface_pool::_clear()
{
	m_ReservedCnt = 0;
	m_SurfaceAll.clear();

	if(m_mfxResponse.NumFrameActual > 0)
		m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);
	memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
}
