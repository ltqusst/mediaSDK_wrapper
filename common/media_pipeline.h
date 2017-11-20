#ifndef _MEDIAPIPELINE_H_
#define _MEDIAPIPELINE_H_

#include "videoframe_allocator.h"
#include "blocking_queue.h"
#include "surface_pool.h"
#include "bitstreams.h"


class MediaDecoder
{
public:
	MediaDecoder(int output_queue_size);
	virtual ~MediaDecoder();

	void start(const char * file_url, mfxIMPL impl = MFX_IMPL_AUTO, bool drop_on_overflow = false);
	void stop(void);

	typedef std::pair<std::shared_ptr<surface1>, std::shared_ptr<surface1>> Output;

	bool get(Output & r){ return m_outputs.get(r); }
private:
	//the mediaSDK pipeline
	void decode(const char * file_url, mfxIMPL impl, bool drop_on_overflow);

	std::thread *					m_pthread = NULL;
	videoframe_allocator			m_mfxAllocator;
	surface_pool                 	spDEC;
	surface_pool                 	spVPP;
	blocking_queue<Output> 			m_outputs;

	enum Debug{no=0, yes, dec, out};
	Debug							m_debug;
	volatile bool 					m_stop;
};

#endif
