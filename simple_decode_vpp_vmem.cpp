/*****************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or
nondisclosure agreement with Intel Corporation and may not be copied
or disclosed except in accordance with the terms of that agreement.
Copyright(c) 2005-2014 Intel Corporation. All Rights Reserved.

*****************************************************************************/

#include "common_utils.h"
#include "cmd_options.h"

static void usage(CmdOptionsCtx* ctx)
{
    printf(
        "Decodes INPUT and optionally writes OUTPUT.\n"
        "\n"
        "Usage: %s [options] INPUT [OUTPUT]\n", ctx->program);
}

#include <vector>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <thread>
#include <memory>
#include <mutex>
#include <functional>

class mfxVideoFrameAllocator: public mfxFrameAllocator
{
public:
	mfxVideoFrameAllocator(){
		mfxFrameAllocator::pthis = this;
		mfxFrameAllocator::Alloc  = simple_alloc;
		mfxFrameAllocator::Free   = simple_free;
		mfxFrameAllocator::Lock   = simple_lock;
		mfxFrameAllocator::Unlock = simple_unlock;
		mfxFrameAllocator::GetHDL = simple_gethdl;
	}
};

class hddlBitstreamBase: public mfxBitstream
{
public:
	hddlBitstreamBase(const int buffMaxLen = 1024*1024){
	    memset((mfxBitstream*)this, 0, sizeof(mfxBitstream));
	    this->mfxBitstream::MaxLength = buffMaxLen;
	    this->mfxBitstream::Data = new mfxU8[this->mfxBitstream::MaxLength];
	    assert(this->mfxBitstream::Data);
	}
	virtual ~hddlBitstreamBase(){
		if(this->mfxBitstream::Data)
			delete []this->mfxBitstream::Data;
	}
	virtual mfxU32 Feed(void)=0;
	virtual bool IsEnd(void)=0;
};

class hddlBitstreamFile: public hddlBitstreamBase
{
public:
	hddlBitstreamFile(const char * fname, bool bRepeat = false){
		m_fSource = fopen(fname,"rb");
		m_bRepeat = bRepeat;
		assert(m_fSource);
	}
	virtual ~hddlBitstreamFile(){
		if(m_fSource) fclose(m_fSource);
	}

	virtual mfxU32 Feed(void){

		memmove(this->Data, this->Data + this->DataOffset, this->DataLength);
		this->DataOffset = 0;

		mfxU32 nBytesRead = 0;
		mfxU32 nBytesSpace = this->MaxLength - this->DataLength;
		while(!m_bEOS && nBytesRead == 0 && nBytesSpace > 0)
		{
			nBytesRead = (mfxU32) fread(this->Data + this->DataLength, 1, nBytesSpace, m_fSource);

			//printf("nBytesSpace=%d, nBytesRead=%d\n", nBytesSpace, nBytesRead);

			if (0 == nBytesRead){
				if(m_bRepeat)
					fseek(m_fSource, 0, SEEK_SET);
				else
					m_bEOS = true;
			}
		}
		this->TimeStamp ++;
		this->DataLength += nBytesRead;
		return nBytesRead;
	}
	virtual bool IsEnd(void){
		return m_bEOS && this->DataLength == 0;
	}

	FILE *m_fSource = NULL;
	bool m_bEOS = false;
	bool m_bRepeat = false;
};

class hddlSurfacePool;

class hddlSurface1: public mfxFrameSurface1{
public:
	hddlSurface1(const mfxFrameAllocator & mfxAllocator,
                 const mfxFrameInfo * pfmt = NULL):
                	 m_bReserved(false),
					 m_bLockedByAllocator(false),
					 m_mfxAllocator(mfxAllocator)
	{
		memset((mfxFrameSurface1*)this, 0, sizeof(mfxFrameSurface1));
		if(pfmt)
			memcpy(&(this->mfxFrameSurface1::Info), pfmt, sizeof(mfxFrameInfo));
	}
	~hddlSurface1(){
		Unlock();
	}

	//after successfully Lock(), the Pitch,YUV,RGB field of Data member will be set
	mfxStatus Lock(){
		mfxStatus sts = m_mfxAllocator.Lock(m_mfxAllocator.pthis, this->Data.MemId, &this->Data);
		if(sts == MFX_ERR_NONE)
			m_bLockedByAllocator = (true);
		return sts;
	}

	//after successfully Unlock(), the Pitch,YUV,RGB field of Data member will be 0/NULL
	mfxStatus Unlock(){
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
	void Reserve(bool bset){
		m_bReserved = (bset);
	}
	bool IsReserved(void){
		return m_bReserved;
	}

	bool m_bReserved;
	bool m_bLockedByAllocator;
	const mfxFrameAllocator & m_mfxAllocator;

	friend class hddlSurfacePool;
};

class hddlSurfacePool
{
public:
	hddlSurfacePool(const mfxFrameAllocator & mfxAllocator):
		m_mfxAllocator(mfxAllocator){
		memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
		clear();
	}

	~hddlSurfacePool(){
		clear();
	}

	mfxStatus realloc(mfxFrameAllocRequest Request, int cntReserved = 0){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);

		mfxFrameAllocResponse mfxResponse;

		Request.NumFrameMin += cntReserved;
		Request.NumFrameSuggested += cntReserved;

		mfxStatus sts = m_mfxAllocator.Alloc(m_mfxAllocator.pthis, &Request, &mfxResponse);
	    if(MFX_ERR_NONE != sts) return sts;

	    //clear old pool only when new allocation success
	    clear();

	    //replace configuration data
	    m_ReservedMaxCnt = cntReserved;
	    m_mfxResponse = mfxResponse;

	    // Allocate surface headers (mfxFrameSurface1) for decoder
	    for (int i = 0; i < m_mfxResponse.NumFrameActual; i++) {
	    	hddlSurface1 s(m_mfxAllocator, &(Request.Info));
	    	s.Data.MemId = m_mfxResponse.mids[i];
	    	m_SurfaceQueue.push_back(s);
	    }

	    return MFX_ERR_NONE;
	}

	void clear(void){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);

		m_ReservedCnt = 0;
		m_SurfaceQueue.clear();

		if(m_mfxResponse.NumFrameActual > 0)
			m_mfxAllocator.Free(m_mfxAllocator.pthis, &m_mfxResponse);
		memset(&m_mfxResponse, 0, sizeof(m_mfxResponse));
	}

	// Get free raw frame surface
	mfxFrameSurface1 * getfree(bool bVerbose = false){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);

		for (int i = 0; i < m_SurfaceQueue.size(); i++)
			if (0 == m_SurfaceQueue[i].Data.Locked && !m_SurfaceQueue[i].IsReserved()){
				if(bVerbose) printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Locking: %d\n", i);
				return static_cast<mfxFrameSurface1 *>(&(m_SurfaceQueue[i]));
			}
	    return NULL;
	}

	int surfaceID(mfxFrameSurface1 * psurf){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);
		for (int i = 0; i < m_SurfaceQueue.size(); i++)
			if(psurf == &(m_SurfaceQueue[i]))
				return i;
		return -1;
	}

	mfxFrameSurface1 * find(mfxU32 FrameOrder){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);
		for (int i = 0; i < m_SurfaceQueue.size(); i++)
			if(m_SurfaceQueue[i].Data.FrameOrder == FrameOrder){
				return static_cast<mfxFrameSurface1 *>(&(m_SurfaceQueue[i]));
			}
		return NULL;
	}

	//Reserved surface will not be allocated again
	bool reserve(mfxFrameSurface1 * psurf){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);
		// reserved surface is for application thread to consume
		// the output surface in an async-way. also number of reserved surfaces
		// is hard limited so decoding is not affected by the reservation.
		for (int i = 0; i < m_SurfaceQueue.size(); i++){
			if(psurf == &(m_SurfaceQueue[i])){
				//multiple reserve is allowed
				if(m_SurfaceQueue[i].IsReserved()) return true;

				if(m_ReservedCnt < m_ReservedMaxCnt){
					m_SurfaceQueue[i].Reserve(true);
					m_ReservedCnt ++;
					return true;
				}
			}
		}
		return false;
	}

	bool unreserve(mfxFrameSurface1 * psurf){
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);
		for (int i = 0; i < m_SurfaceQueue.size(); i++){
			if(psurf == &(m_SurfaceQueue[i])){
				//multiple reserve is allowed
				if(!m_SurfaceQueue[i].IsReserved())
					return true; //already unreserved

				m_SurfaceQueue[i].Reserve(false);
				if(m_ReservedCnt > 0)
					m_ReservedCnt --;
				return true;
			}
		}
		return false; //not found
	}

	//
	void debug(void)
	{
		std::lock_guard<std::recursive_mutex> guard(m_SurfaceQMutex);
		printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Locked:");
		for (int i = 0; i < m_SurfaceQueue.size(); i++)
		{
			if(m_SurfaceQueue[i].Data.Locked)
				printf("%s%d,", m_SurfaceQueue[i].IsReserved()?"R":"", i);
		}

		printf(" Free: ");
		for (int i = 0; i < m_SurfaceQueue.size(); i++){
			if(!m_SurfaceQueue[i].Data.Locked && !m_SurfaceQueue[i].IsReserved())
				printf("%d,", i);
		}

		printf(" Reserved Free: ");
		for (int i = 0; i < m_SurfaceQueue.size(); i++){
			if(!m_SurfaceQueue[i].Data.Locked && m_SurfaceQueue[i].IsReserved())
				printf("%d,", i);
		}
		printf("\n");
	}


	mfxFrameAllocResponse   		m_mfxResponse;
	const mfxFrameAllocator &      	m_mfxAllocator;

	std::vector<hddlSurface1> 		m_SurfaceQueue;
	std::recursive_mutex            m_SurfaceQMutex;
	int                             m_ReservedMaxCnt;
	int                             m_ReservedCnt;
};



class MediaDecoder
{
public:
	MediaDecoder();
	virtual ~MediaDecoder();

	//We use c++11 function template wrapper callback to accept:
	//    (lambda)/(functor)/std::bind()/(traditional function pointer)
	typedef std::function<void(MediaDecoder * pthis, hddlSurface1 * pdec, hddlSurface1 * pvpp)> newframe_callback;

	void start(const char * file_url, mfxIMPL impl, int surface_reserve_cnt, newframe_callback fcallback);
	void stop(void);

	//the mediaSDK pipeline
	void decode(const char * file_url, mfxIMPL impl, int surface_reserve_cnt, newframe_callback fcallback);

	bool return_frame(hddlSurface1 * p);
private:
	std::thread *					m_pthread = NULL;
	mfxFrameAllocator 				m_mfxAllocator;
	hddlSurfacePool                 spDEC;
	hddlSurfacePool                 spVPP;
};

MediaDecoder::MediaDecoder():
		spDEC(m_mfxAllocator),
		spVPP(m_mfxAllocator){
}
MediaDecoder::~MediaDecoder(){
	stop();
}

void MediaDecoder::start(const char * file_url, mfxIMPL impl, int surface_reserve_cnt, newframe_callback fcallback)
{
	if(m_pthread){
		fprintf(stderr,"Error, thread is already running\n");
	}else{
		m_pthread = new std::thread(&MediaDecoder::decode, this, file_url, impl, surface_reserve_cnt, fcallback);
	}
}
void MediaDecoder::stop(void)
{
	if(m_pthread){
		if(m_pthread->joinable())
			m_pthread->join();
		m_pthread = NULL;
	}
}
bool MediaDecoder::return_frame(hddlSurface1 * p)
{
	if(!spDEC.unreserve(p))
	{
		if(!spVPP.unreserve(p))
		{
			return false;
		}
	}
	return true;
}

void MediaDecoder::decode(const char * file_url, mfxIMPL impl, int surface_reserve_cnt, newframe_callback fcallback)
{
	hddlBitstreamFile Bs(file_url, false);

#define MD_CHECK_RESULT(sts, value, predix, goto_where)     \
	if(sts != value) {\
	printf("(%s:%d) "#predix" return error %d \n", __FILE__, __LINE__, sts);\
	return;\
	}

	mfxStatus sts = MFX_ERR_NONE;

	mfxVersion ver = { {0, 1} };
    MFXVideoSession session;

    sts = Initialize(impl, ver, &session, &m_mfxAllocator);
    MD_CHECK_RESULT(sts , MFX_ERR_NONE,"Initialize", DECODE_EXIT0);

    // Create Media SDK decoder
    MFXVideoDECODE mfxDEC(session);
    // Create Media SDK VPP component
    MFXVideoVPP mfxVPP(session);

    // Set required video parameters for decode
    mfxVideoParam mfxVideoParams;
    memset(&mfxVideoParams, 0, sizeof(mfxVideoParams));
    mfxVideoParams.mfx.CodecId = MFX_CODEC_AVC;
    //mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    // Prepare Media SDK bit stream buffer
    // - Arbitrary buffer size for this example
    // Read a chunk of data from stream file into bit stream buffer
    // - Parse bit stream, searching for header and fill video parameters structure
    // - Abort if bit stream header is not found in the first bit stream buffer chunk
    Bs.Feed();

    sts = mfxDEC.DecodeHeader(&Bs, &mfxVideoParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MD_CHECK_RESULT(sts , MFX_ERR_NONE, "DecodeHeader", DECODE_EXIT1);

    // Initialize VPP parameters
    // - For simplistic memory management, system memory surfaces are used to store the raw frames
    //   (Note that when using HW acceleration D3D surfaces are prefered, for better performance)
    mfxVideoParam VPPParams;
    memset(&VPPParams, 0, sizeof(VPPParams));
    // Input data
    VPPParams.vpp.In.FourCC = MFX_FOURCC_NV12;
    VPPParams.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.In.CropX = 0;
    VPPParams.vpp.In.CropY = 0;
    VPPParams.vpp.In.CropW = mfxVideoParams.mfx.FrameInfo.CropW;
    VPPParams.vpp.In.CropH = mfxVideoParams.mfx.FrameInfo.CropH;
    VPPParams.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.In.FrameRateExtN = 30;
    VPPParams.vpp.In.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.In.Width = MSDK_ALIGN16(VPPParams.vpp.In.CropW);
    VPPParams.vpp.In.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.In.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.In.CropH);
    // Output data
    VPPParams.vpp.Out.FourCC = MFX_FOURCC_RGB4;
    VPPParams.vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.Out.CropX = 0;
    VPPParams.vpp.Out.CropY = 0;
    VPPParams.vpp.Out.CropW = 448;   // Resize to half size resolution
    VPPParams.vpp.Out.CropH = 448;
    VPPParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.Out.FrameRateExtN = 30;
    VPPParams.vpp.Out.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.Out.Width = MSDK_ALIGN16(VPPParams.vpp.Out.CropW);
    VPPParams.vpp.Out.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.Out.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.Out.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.Out.CropH);

    //VPPParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    VPPParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    // Query number of required surfaces for decoder
    mfxFrameAllocRequest DecRequest;
    memset(&DecRequest, 0, sizeof(DecRequest));
    sts = mfxDEC.QueryIOSurf(&mfxVideoParams, &DecRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MD_CHECK_RESULT(sts, MFX_ERR_NONE, "mfxDEC.QueryIOSurf", DECODE_EXIT2);

    // Query number of required surfaces for VPP
    mfxFrameAllocRequest VPPRequest[2];     // [0] - in, [1] - out
    memset(&VPPRequest, 0, sizeof(mfxFrameAllocRequest) * 2);
    sts = mfxVPP.QueryIOSurf(&VPPParams, VPPRequest);
    MD_CHECK_RESULT(sts, MFX_ERR_NONE, "mfxVPP.QueryIOSurf", DECODE_EXIT2);

    // Determine the required number of surfaces for decoder output (VPP input) and for VPP output
    mfxU16 nSurfNumDecVPP = DecRequest.NumFrameSuggested + VPPRequest[0].NumFrameSuggested;
    mfxU16 nSurfNumVPPOut = VPPRequest[1].NumFrameSuggested;

    mfxPrintReq(&DecRequest, "DecRequest");
    mfxPrintReq(&VPPRequest[0], "VPPRequest[0]");
    mfxPrintReq(&VPPRequest[1], "VPPRequest[1]");

    //Surface pool
    spDEC.realloc(DecRequest, surface_reserve_cnt);
    spVPP.realloc(VPPRequest[1], surface_reserve_cnt);

    // Initialize the Media SDK decoder
    sts = mfxDEC.Init(&mfxVideoParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MD_CHECK_RESULT(sts, MFX_ERR_NONE, "mfxDEC.Init", DECODE_EXIT3);

    // Initialize Media SDK VPP
    sts = mfxVPP.Init(&VPPParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MD_CHECK_RESULT(sts, MFX_ERR_NONE, "mfxVPP.Init", DECODE_EXIT4);


    mfxSyncPoint syncpD;
    mfxSyncPoint syncpV;

    mfxU32 nFrame = 0;

    bool bRunningDEC = true;
    bool bRunningVPP = true;

    int dec_id = 0;
    int vpp_id = 0;

    mfxFrameSurface1* pmfxWorkSurfaceDEC = NULL;

    // Main loop
    while (bRunningDEC || bRunningVPP) {

    	// Here we use fully Synced operation instead of ASync to make robust & easy pipeline
    	// (performance penalty is acceptable for multi-channel application)

    	//1st stage, DEC

    	mfxFrameSurface1* pmfxOutSurface = NULL;

    	bool bOutReadyDEC = false;
    	while(bRunningDEC && !bOutReadyDEC){

			// Decode a frame asychronously (returns immediately)

    		// Update work surface
    		if(pmfxWorkSurfaceDEC == NULL) {
    			pmfxWorkSurfaceDEC = spDEC.getfree();
    			if(pmfxWorkSurfaceDEC == NULL){
    				fprintf(stderr, "%s:%d spDEC.getfree() return NULL\n", __FILE__, __LINE__);
    				goto DECODE_LOOPEND;
    			}
    		}

			sts = mfxDEC.DecodeFrameAsync(Bs.IsEnd()?NULL:&Bs, pmfxWorkSurfaceDEC, &pmfxOutSurface, &syncpD);

    		switch(sts)
    		{
    		case MFX_WRN_DEVICE_BUSY:
    			MSDK_SLEEP(1); //1ms delay
    			break;
    		case MFX_ERR_MORE_DATA:
				if(Bs.IsEnd()) {
					//printf("WARNING: Bs is end and decoder still requires more data\n");
					bRunningDEC = false;
					break;
				}
				Bs.Feed();
    			break;
    		case MFX_ERR_MORE_SURFACE:
    			pmfxWorkSurfaceDEC = NULL;
    			break;
    		case MFX_WRN_VIDEO_PARAM_CHANGED:
    			//TODO
    			// Drain all frames by using NULL bitstream
    			// call MFXVideoDECODE_GetVideoParam() realloc surface pool
    			// call MFXVideoDECODE_Close() to restart
    			break;
    		default:
    			//other error, we cannot handle
    			break;
    		}

            // Ignore warnings if output is available,
            // if no output and no action required just repeat the DecodeFrameAsync call
    		if (MFX_ERR_NONE <= sts && syncpD){
    			//output from DEC is available,
				//check output surface ensure Robustness by reset on corruption
				//don't worry too much about performance penalty on Async,
				//because throughput on multiple channel will keep whole Graphic HW busy enough
				//we only need to make sure the performance is good enough for 1 channel of video.
				sts = session.SyncOperation(syncpD, 60000);
				if(sts == MFX_ERR_NONE){
					if(pmfxOutSurface->Data.Corrupted)
						mfxDEC.Reset(&mfxVideoParams);
					else
						bOutReadyDEC = true;
				}
    		}
    	}
    	//MFX_FRAMEORDER_UNKNOWN
		//MFX_TIMESTAMP_UNKNOWN

    	//either decoder is done or one frame is ready for further operation
    	//spDEC.debug();
    	//if((dec_id & 0xFFF) == 0)
    	if(0)
    	{
			printf("%s:%d, %d,%d, id:dec_%d,vpp_%d, Outsurface:%d, Locked:%d, FrameOrder:0x%X, Timestamp:%llu\n",
					__FILE__,__LINE__, bRunningDEC, bOutReadyDEC, dec_id, vpp_id,
					pmfxOutSurface?spDEC.surfaceID(pmfxOutSurface):-1,
					pmfxOutSurface?pmfxOutSurface->Data.Locked:-1,
					pmfxOutSurface?pmfxOutSurface->Data.FrameOrder:-1,
					pmfxOutSurface?pmfxOutSurface->Data.TimeStamp:-1);
			//usleep(1000*20);
    	}

    	if(pmfxOutSurface) {
    		static_cast<hddlSurface1*>(pmfxOutSurface)->m_FrameNumber = dec_id;
    		spDEC.reserve(pmfxOutSurface);
    	}

    	dec_id ++;

    	//2nd stage VPP
    	if(bOutReadyDEC || !bRunningDEC){
    		//
    		mfxFrameSurface1* pmfxOutSurfaceVPP = spVPP.getfree();
			if(pmfxOutSurfaceVPP == NULL){
				fprintf(stderr, "%s:%d spVPP.getfree() return NULL\n", __FILE__, __LINE__);
				goto DECODE_LOOPEND;
			}

            // until some meaningful result is got
            do
            {
                // Process a frame asychronously (returns immediately)
                sts = mfxVPP.RunFrameVPPAsync(pmfxOutSurface, pmfxOutSurfaceVPP, NULL, &syncpV);

                if (MFX_WRN_DEVICE_BUSY == sts)
                	MSDK_SLEEP(1);  // wait if device is busy
                //if(MFX_ERR_NONE != sts) printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %d, %p\n", sts, syncpV);

            }while(MFX_ERR_NONE < sts && !syncpV);

            //printf("%s:%d, %d,%d, id:dec_  ,vpp_%d\n",__FUNCTION__,__LINE__, bRunningDEC, bOutReadyDEC, vpp_id);

            switch(sts)
            {
            case MFX_ERR_MORE_DATA:
            	if(pmfxOutSurface == NULL){
            		//fprintf(stderr,"WARNING: Decoder is done while VPP still requires more data\n");
            		bRunningVPP = false;
            	}
            	break;
            case MFX_ERR_MORE_SURFACE:
                // Not relevant for the illustrated workload! Therefore not handled.
                // Relevant for cases when VPP produces more frames at output than consumes at input.
            	// E.g. framerate conversion 30 fps -> 60 fps
				fprintf(stderr,"%s:%d Cannot handle MFX_ERR_MORE_SURFACE\n", __FILE__, __LINE__);
				goto DECODE_LOOPEND;
            	break;
            default:
            	MSDK_BREAK_ON_ERROR(sts);
            }

            if (MFX_ERR_NONE <= sts && syncpV){
            	// ignore warnings if output is available
   				// Synchronize. Wait until decoded frame is ready
    			if (MFX_ERR_NONE == session.SyncOperation(syncpV, 60000)) {
    				++nFrame;
    				//reserve VPP output surface & find corresponding decode output
    				spVPP.reserve(pmfxOutSurfaceVPP);
    				static_cast<hddlSurface1*>(pmfxOutSurfaceVPP)->m_FrameNumber = vpp_id;

    				mfxFrameSurface1 * pmfxOutSurfaceVPPDEC = spDEC.find(pmfxOutSurfaceVPP->Data.FrameOrder);
    				if(pmfxOutSurfaceVPPDEC)
    					spDEC.reserve(pmfxOutSurfaceVPPDEC);
    				else
    				{
    					fprintf(stderr,"%s:%d Cannot find decoder output by FrameOrder\n", __FILE__, __LINE__);
    					goto DECODE_LOOPEND;
    				}

    				fcallback(this,
    						static_cast<hddlSurface1 *>(pmfxOutSurfaceVPPDEC),
    						static_cast<hddlSurface1 *>(pmfxOutSurfaceVPP));

    				vpp_id++;
    			}
            }

    	}
    }

DECODE_LOOPEND:
    mfxVPP.Close();
DECODE_EXIT4:
	mfxDEC.Close();

DECODE_EXIT3:
DECODE_EXIT2:
DECODE_EXIT1:
    Release();
DECODE_EXIT0:
	return;
}


int main(int argc, char** argv)
{
    mfxStatus sts = MFX_ERR_NONE;
    bool bEnableOutput; // if true, removes all YUV file writing
    CmdOptions options;

    // =====================================================================
    // Intel Media SDK decode pipeline setup
    // - In this example we are decoding an AVC (H.264) stream
    // - For simplistic memory management, system memory surfaces are used to store the decoded frames
    //   (Note that when using HW acceleration video surfaces are prefered, for better performance)
    //   VPP frame processing included in pipeline (resize)

    // Read options from the command line (if any is given)
    memset(&options, 0, sizeof(CmdOptions));
    options.ctx.options = OPTIONS_DECODE;
    options.ctx.usage = usage;
    // Set default values:
    options.values.impl = MFX_IMPL_AUTO_ANY;

    // here we parse options
    ParseOptions(argc, argv, &options);

    if (!options.values.SourceName[0]) {
        printf("error: source file name not set (mandatory)\n");
        return -1;
    }

    bEnableOutput = (options.values.SinkName[0] != '\0');

    // Create output YUV file
    FILE* fSink = NULL;
    if (bEnableOutput) {
        MSDK_FOPEN(fSink, options.values.SinkName, "wb");
        MSDK_CHECK_POINTER(fSink, MFX_ERR_NULL_PTR);
    }

    mfxU32 nFrame = 0;

    auto newframe_cb = [&](MediaDecoder * pm, hddlSurface1 * pdec, hddlSurface1 * pvpp){
    		while (bEnableOutput) {
    			mfxStatus sts = pvpp->Lock();
    			MSDK_BREAK_ON_ERROR(sts) ;

    			mfxPrintFrameInfo(pvpp->Info);

    			sts = WriteRawFrame(static_cast<mfxFrameSurface1*>(pvpp), fSink);
    			MSDK_BREAK_ON_ERROR(sts);

    			MSDK_BREAK_ON_ERROR(pvpp->Unlock());
    			printf("Frame number (DEC, VPP): %d, %d \n", pdec->m_FrameNumber, pvpp->m_FrameNumber);
    			break;
    		}
    		nFrame ++;
    		pm->return_frame(pdec);
    		pm->return_frame(pvpp);
    };

    MediaDecoder m;

    mfxTime tStart, tEnd;
    mfxGetTime(&tStart);

    m.start(options.values.SourceName, options.values.impl, 1, newframe_cb);
    m.stop();

    mfxGetTime(&tEnd);
    double elapsed = TimeDiffMsec(tEnd, tStart) / 1000;
    double fps = ((double)nFrame / elapsed);
    printf("\nTotal Frames: %d, Execution time: %3.2f s (%3.2f fps)\n", nFrame, elapsed, fps);

    if (fSink) fclose(fSink);
}

#if 0

//======================================
class hddlFrame
{
public:

};
class hddlPipeline
{
public:
	typedef void (*FrameCallback)(hddlFrame * pf);
	virtual decode(const char * file_url, FrameCallback cb) = 0;
	virtual free(hddlFrame * pf) = 0;
private:
	hddlPipeline(){};
	static hddlPipeline * create(void);
	//start decode thread, callback is registered
	//in callback, user will be notified which
};

#endif






#if 0
int main(int argc, char** argv)
{
    mfxStatus sts = MFX_ERR_NONE;
    bool bEnableOutput; // if true, removes all YUV file writing
    CmdOptions options;

    // =====================================================================
    // Intel Media SDK decode pipeline setup
    // - In this example we are decoding an AVC (H.264) stream
    // - For simplistic memory management, system memory surfaces are used to store the decoded frames
    //   (Note that when using HW acceleration video surfaces are prefered, for better performance)
    //   VPP frame processing included in pipeline (resize)

    // Read options from the command line (if any is given)
    memset(&options, 0, sizeof(CmdOptions));
    options.ctx.options = OPTIONS_DECODE;
    options.ctx.usage = usage;
    // Set default values:
    options.values.impl = MFX_IMPL_AUTO_ANY;

    // here we parse options
    ParseOptions(argc, argv, &options);

    if (!options.values.SourceName[0]) {
        printf("error: source file name not set (mandatory)\n");
        return -1;
    }

    bEnableOutput = (options.values.SinkName[0] != '\0');
    // Open input H.264 elementary stream (ES) file
    hddlBitstreamBase * pBS = new hddlBitstreamFile(options.values.SourceName, false);

    //FILE* fSource;
    //MSDK_FOPEN(fSource, options.values.SourceName, "rb");
    //MSDK_CHECK_POINTER(fSource, MFX_ERR_NULL_PTR);

    // Create output YUV file
    FILE* fSink = NULL;
    if (bEnableOutput) {
        MSDK_FOPEN(fSink, options.values.SinkName, "wb");
        MSDK_CHECK_POINTER(fSink, MFX_ERR_NULL_PTR);
    }

    // Initialize Intel Media SDK session
    // - MFX_IMPL_AUTO_ANY selects HW acceleration if available (on any adapter)
    // - Version 1.0 is selected for greatest backwards compatibility.
    //   If more recent API features are needed, change the version accordingly
    mfxIMPL impl = options.values.impl;
    mfxVersion ver = { {0, 1} };
    MFXVideoSession session;
    mfxFrameAllocator mfxAllocator;
    sts = Initialize(impl, ver, &session, &mfxAllocator);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Create Media SDK decoder
    MFXVideoDECODE mfxDEC(session);
    // Create Media SDK VPP component
    MFXVideoVPP mfxVPP(session);

    // Set required video parameters for decode
    mfxVideoParam mfxVideoParams;
    memset(&mfxVideoParams, 0, sizeof(mfxVideoParams));
    mfxVideoParams.mfx.CodecId = MFX_CODEC_AVC;
    //mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    mfxVideoParams.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    // Prepare Media SDK bit stream buffer
    // - Arbitrary buffer size for this example
    // Read a chunk of data from stream file into bit stream buffer
    // - Parse bit stream, searching for header and fill video parameters structure
    // - Abort if bit stream header is not found in the first bit stream buffer chunk
    pBS->Feed();

    sts = mfxDEC.DecodeHeader(pBS, &mfxVideoParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize VPP parameters
    // - For simplistic memory management, system memory surfaces are used to store the raw frames
    //   (Note that when using HW acceleration D3D surfaces are prefered, for better performance)
    mfxVideoParam VPPParams;
    memset(&VPPParams, 0, sizeof(VPPParams));
    // Input data
    VPPParams.vpp.In.FourCC = MFX_FOURCC_NV12;
    VPPParams.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.In.CropX = 0;
    VPPParams.vpp.In.CropY = 0;
    VPPParams.vpp.In.CropW = mfxVideoParams.mfx.FrameInfo.CropW;
    VPPParams.vpp.In.CropH = mfxVideoParams.mfx.FrameInfo.CropH;
    VPPParams.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.In.FrameRateExtN = 30;
    VPPParams.vpp.In.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.In.Width = MSDK_ALIGN16(VPPParams.vpp.In.CropW);
    VPPParams.vpp.In.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.In.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.In.CropH);
    // Output data
    VPPParams.vpp.Out.FourCC = MFX_FOURCC_RGB4;
    VPPParams.vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.Out.CropX = 0;
    VPPParams.vpp.Out.CropY = 0;
    VPPParams.vpp.Out.CropW = 448;   // Resize to half size resolution
    VPPParams.vpp.Out.CropH = 448;
    VPPParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.Out.FrameRateExtN = 30;
    VPPParams.vpp.Out.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.Out.Width = MSDK_ALIGN16(VPPParams.vpp.Out.CropW);
    VPPParams.vpp.Out.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.Out.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.Out.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.Out.CropH);

    //VPPParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    VPPParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    // Query number of required surfaces for decoder
    mfxFrameAllocRequest DecRequest;
    memset(&DecRequest, 0, sizeof(DecRequest));
    sts = mfxDEC.QueryIOSurf(&mfxVideoParams, &DecRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Query number of required surfaces for VPP
    mfxFrameAllocRequest VPPRequest[2];     // [0] - in, [1] - out
    memset(&VPPRequest, 0, sizeof(mfxFrameAllocRequest) * 2);
    sts = mfxVPP.QueryIOSurf(&VPPParams, VPPRequest);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Determine the required number of surfaces for decoder output (VPP input) and for VPP output
    mfxU16 nSurfNumDecVPP = DecRequest.NumFrameSuggested + VPPRequest[0].NumFrameSuggested;
    mfxU16 nSurfNumVPPOut = VPPRequest[1].NumFrameSuggested;

    mfxPrintReq(&DecRequest, "DecRequest");
    mfxPrintReq(&VPPRequest[0], "VPPRequest[0]");
    mfxPrintReq(&VPPRequest[1], "VPPRequest[1]");

    //Surface pool
    hddlSurfacePool spDEC(mfxAllocator);
    spDEC.realloc(DecRequest, 10);

    hddlSurfacePool spVPP(mfxAllocator);
    spVPP.realloc(VPPRequest[1]);

    // Initialize the Media SDK decoder
    sts = mfxDEC.Init(&mfxVideoParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize Media SDK VPP
    sts = mfxVPP.Init(&VPPParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // ===============================================================
    // Start decoding the frames from the stream
    //
    mfxTime tStart, tEnd;
    mfxGetTime(&tStart);

    mfxSyncPoint syncpD;
    mfxSyncPoint syncpV;

    mfxU32 nFrame = 0;


    bool bRunningDEC = true;
    bool bRunningVPP = true;

    int dec_id = 0;
    int vpp_id = 0;

    mfxFrameSurface1* pmfxWorkSurfaceDEC = NULL;

    //
    // Stage 1: Main decoding loop
    //
    while (bRunningDEC || bRunningVPP) {

    	// Here we use fully Synced operation instead of ASync
    	// to make robust & easy pipeline (performance penalty is acceptable for multi-channel application)
    	//

    	//1st stage, DEC
    	mfxFrameSurface1* pmfxOutSurface = NULL;

    	bool bOutReadyDEC = false;
    	while(bRunningDEC && !bOutReadyDEC){

			// Decode a frame asychronously (returns immediately)

    		//Update work surface
    		if(pmfxWorkSurfaceDEC == NULL) {
    			pmfxWorkSurfaceDEC = spDEC.getfree();
    			MSDK_CHECK_ERROR(NULL, pmfxWorkSurfaceDEC, MFX_ERR_MEMORY_ALLOC);
    		}

			sts = mfxDEC.DecodeFrameAsync(pBS->IsEnd()?NULL:pBS, pmfxWorkSurfaceDEC, &pmfxOutSurface, &syncpD);

    		switch(sts)
    		{
    		case MFX_WRN_DEVICE_BUSY:
    			MSDK_SLEEP(1); //1ms delay
    			break;
    		case MFX_ERR_MORE_DATA:
				if(pBS->IsEnd()) {
					printf("WARNING: Bs is end and decoder still requires more data\n");
					bRunningDEC = false;
					break;
				}
				pBS->Feed();
    			break;
    		case MFX_ERR_MORE_SURFACE:
    			pmfxWorkSurfaceDEC = NULL;
    			break;
    		case MFX_WRN_VIDEO_PARAM_CHANGED:
    			break;
    		default:
    			//other error, we cannot handle
    			break;
    		}

            // Ignore warnings if output is available,
            // if no output and no action required just repeat the DecodeFrameAsync call
    		if (MFX_ERR_NONE <= sts && syncpD){
    			//output from DEC is available,
				//check output surface ensure Robustness by reset on corruption
				//don't worry too much about performance penalty on Async,
				//because throughput on multiple channel will keep whole Graphic HW busy enough
				//we only need to make sure the performance is good enough for 1 channel of video.
				sts = session.SyncOperation(syncpD, 60000);
				if(sts == MFX_ERR_NONE){
					if(pmfxOutSurface->Data.Corrupted)
						mfxDEC.Reset(&mfxVideoParams);
					else
						bOutReadyDEC = true;
				}
    		}
    	}
    	//MFX_FRAMEORDER_UNKNOWN
		//MFX_TIMESTAMP_UNKNOWN

    	//either decoder is done or one frame is ready for further operation
    	//spDEC.debug();
    	//if((dec_id & 0xFFF) == 0)
    	if(1)
    	{
			printf("%s:%d, %d,%d, id:dec_%d,vpp_%d, Outsurface:%d, Locked:%d, FrameOrder:0x%X, Timestamp:%llu\n",
					__FUNCTION__,__LINE__, bRunningDEC, bOutReadyDEC, dec_id, vpp_id,
					pmfxOutSurface?spDEC.surfaceID(pmfxOutSurface):-1,
					pmfxOutSurface?pmfxOutSurface->Data.Locked:-1,
					pmfxOutSurface?pmfxOutSurface->Data.FrameOrder:-1,
					pmfxOutSurface?pmfxOutSurface->Data.TimeStamp:-1);
			usleep(1000*20);
    	}

    	if(pmfxOutSurface) spDEC.reserve(pmfxOutSurface);

    	dec_id ++;

    	//2nd stage VPP
    	if(bOutReadyDEC || !bRunningDEC){
    		//
    		mfxFrameSurface1* pmfxOutSurfaceVPP = spVPP.getfree();
            MSDK_CHECK_ERROR(NULL, pmfxOutSurfaceVPP, MFX_ERR_MEMORY_ALLOC);

            // until some meaningful result is got
            do
            {
                // Process a frame asychronously (returns immediately)
                sts = mfxVPP.RunFrameVPPAsync(pmfxOutSurface, pmfxOutSurfaceVPP, NULL, &syncpV);

                if (MFX_WRN_DEVICE_BUSY == sts)
                	MSDK_SLEEP(1);  // wait if device is busy
                if(MFX_ERR_NONE != sts)
                	printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %d, %p\n", sts, syncpV);
            }while(MFX_ERR_NONE < sts && !syncpV);

            //printf("%s:%d, %d,%d, id:dec_  ,vpp_%d\n",__FUNCTION__,__LINE__, bRunningDEC, bOutReadyDEC, vpp_id);

            switch(sts)
            {
            case MFX_ERR_MORE_DATA:
            	if(pmfxOutSurface == NULL){
            		printf("WARNING: Decoder is done while VPP still requires more data\n");
            		bRunningVPP = false;
            	}
            	break;
            case MFX_ERR_MORE_SURFACE:
                // Not relevant for the illustrated workload! Therefore not handled.
                // Relevant for cases when VPP produces more frames at output than consumes at input.
            	// E.g. framerate conversion 30 fps -> 60 fps
            	break;
            default:
            	MSDK_BREAK_ON_ERROR(sts);
            }

            if (MFX_ERR_NONE <= sts && syncpV){
            	// ignore warnings if output is available
   				// Synchronize. Wait until decoded frame is ready
    			if (MFX_ERR_NONE == session.SyncOperation(syncpV, 60000)) {
    				++nFrame;
    				vpp_id++;
    				if (bEnableOutput) {
    					sts = mfxAllocator.Lock(mfxAllocator.pthis, pmfxOutSurfaceVPP->Data.MemId, &(pmfxOutSurfaceVPP->Data));
    					MSDK_BREAK_ON_ERROR(sts);

    					mfxPrintFrameInfo(pmfxOutSurfaceVPP->Info);

    					sts = WriteRawFrame(pmfxOutSurfaceVPP, fSink);
    					MSDK_BREAK_ON_ERROR(sts);

    					sts = mfxAllocator.Unlock(mfxAllocator.pthis, pmfxOutSurfaceVPP->Data.MemId, &(pmfxOutSurfaceVPP->Data));
    					MSDK_BREAK_ON_ERROR(sts);

    					printf("Frame number: %d\r", nFrame);
    				}
    			}
            }

    	}
    }

    // MFX_ERR_MORE_DATA means that file has ended, need to go to buffering loop, exit in case of other errors
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    mfxGetTime(&tEnd);
    double elapsed = TimeDiffMsec(tEnd, tStart) / 1000;
    double fps = ((double)nFrame / elapsed);
    printf("\nTotal Frames: %d, Execution time: %3.2f s (%3.2f fps)\n", nFrame, elapsed, fps);

    // ===================================================================
    // Clean up resources
    //  - It is recommended to close Media SDK components first, before releasing allocated surfaces, since
    //    some surfaces may still be locked by internal Media SDK resources.

    mfxDEC.Close();
    mfxVPP.Close();
    // session closed automatically on destruction

    //for (int i = 0; i < nSurfNumDecVPP; i++)delete pmfxSurfaces[i];
    //for (int i = 0; i < nSurfNumVPPOut; i++)delete pmfxSurfaces2[i];
	//mfxAllocator.Free(mfxAllocator.pthis, &mfxDecResponse);
	//mfxAllocator.Free(mfxAllocator.pthis, &mfxVPPResponse);

    //MSDK_SAFE_DELETE_ARRAY(pmfxSurfaces);
    //MSDK_SAFE_DELETE_ARRAY(pmfxSurfaces2);
    //MSDK_SAFE_DELETE_ARRAY(surfaceBuffers);
    //MSDK_SAFE_DELETE_ARRAY(surfaceBuffers2);
    //MSDK_SAFE_DELETE_ARRAY(mfxBS.Data);

    delete pBS; //fclose(fSource);
    if (fSink) fclose(fSink);


    Release();

    return 0;
}

#endif
