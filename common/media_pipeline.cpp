
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

#include <stdlib.h>

#include "media_pipeline.h"
#include "common_utils.h"

#include <string.h>
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

MediaDecoder::MediaDecoder(int output_queue_size):
		spDEC(m_mfxAllocator),
		spVPP(m_mfxAllocator),
		m_outputs(output_queue_size),
		m_debug(Debug::no)
{
	char * pdebug = getenv("MD_DEBUG");
	if(pdebug){
		if(strcmp(pdebug,"yes") == 0) m_debug = Debug::yes;
		if(strcmp(pdebug,"dec") == 0) m_debug = Debug::dec;
		if(strcmp(pdebug,"out") == 0) m_debug = Debug::out;
		if(strcmp(pdebug,"st") == 0) m_debug = Debug::st;
	}
}

MediaDecoder::~MediaDecoder()
{
	stop();
}

void MediaDecoder::start(const char * file_url, mfxIMPL impl, bool drop_on_overflow)
{
	static std::atomic<int> g_tty_color(0);

	int color = g_tty_color.fetch_add(1);

	switch(color % 6){
	case 0: m_tty_color = ANSI_COLOR_BLUE; break;
	case 1: m_tty_color = ANSI_COLOR_GREEN; break;
	case 2: m_tty_color = ANSI_COLOR_YELLOW; break;
	case 3: m_tty_color = ANSI_COLOR_RED; break;
	case 4: m_tty_color = ANSI_COLOR_MAGENTA; break;
	case 5: m_tty_color = ANSI_COLOR_CYAN; break;
	}


	if(m_pthread){
		fprintf(stderr,"Error, thread is already running\n");
	}else{
		m_stop = false;
		m_pthread = new std::thread(&MediaDecoder::decode, this, file_url, impl, drop_on_overflow);
	}
}
void MediaDecoder::stop(void)
{
	if(m_pthread){
		m_stop = true;

		//drain the queue, or decode thread may blocked
		Output out_ignore;
		while(get(out_ignore));

		if(m_pthread->joinable())
			m_pthread->join();
		m_pthread = NULL;
	}
}

void MediaDecoder::decode(const char * file_url, mfxIMPL impl, bool drop_on_overflow)
{
	hddlBitstreamFile Bs(file_url, false);

#define MD_CHECK_RESULT(sts, value, predix, goto_where)     \
	if(sts != value) {\
	printf("(%s:%d) "#predix" return error %d \n", __FILENAME__, __LINE__, sts);\
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

    //Surface pool, here we reserved few more frames because:
    //   DEC may advance 2 frames before blocking-put into queue
    //   VPP may advance 1 frames before blocking-put into queue
    spDEC.realloc(DecRequest, 		m_outputs.size_limit()+2);
    spVPP.realloc(VPPRequest[1], 	m_outputs.size_limit()+1);

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

    bool bRunningDEC = true;
    bool bRunningVPP = true;

    int dec_id = 0;
    int vpp_id = 0;
    int dropped_cnt = 0;


    unsigned int st_tick = 0;
    auto t_last = std::chrono::steady_clock::now();
    // Main loop
    while ((bRunningDEC || bRunningVPP) && (!m_stop)) {

    	if(m_debug == Debug::st)
    	{
    		auto t_cur = std::chrono::steady_clock::now();
    		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_cur - t_last);

    		if(ms.count() > 1000){
    			st_tick += ms.count();

    			t_last = t_cur;

    			printf("%s[%d]thread 0x%08X, status: dec %-8d vpp %-8d dropped %-8d fps %d effective fps %d\n" ANSI_COLOR_RESET,
    					m_tty_color,
    					st_tick/1000, (std::this_thread::get_id()),
    					dec_id, vpp_id, dropped_cnt,
						(vpp_id * 1000/ st_tick), ((vpp_id - dropped_cnt) * 1000/ st_tick)
						);
    		}
    	}
    	// Here we use fully Synced operation instead of ASync to make robust & easy pipeline
    	// (performance penalty is acceptable for multi-channel application)

    	//1st stage, DEC
    	surface1 *    phddlSurfaceDEC = NULL;
    	surface1 *    phddlSurfaceVPP = NULL;

    	bool bOutReadyDEC = false;
    	while(bRunningDEC && !bOutReadyDEC){

			// Decode a frame asychronously (returns immediately)

    		mfxFrameSurface1* pmfxSurfaceWork = NULL;
    		mfxFrameSurface1* pmfxSurfaceOut = NULL;

    		// Just provide one free work surface each time call DecodeFrameAsync
    		// it will be locked by DecodeFrameAsync() when necessary
    		pmfxSurfaceWork = spDEC.getfree();

			if(pmfxSurfaceWork == NULL){
				fprintf(stderr, "%s:%d spDEC.getfree() return NULL\n", __FILENAME__, __LINE__);
				goto DECODE_LOOPEND;
			}

			sts = mfxDEC.DecodeFrameAsync(Bs.IsEnd()?NULL:&Bs, pmfxSurfaceWork, &pmfxSurfaceOut, &syncpD);
			phddlSurfaceDEC = static_cast<surface1*>(pmfxSurfaceOut);

    		switch(sts)
    		{
    		case MFX_WRN_DEVICE_BUSY:
    			MSDK_SLEEP(1); //1ms delay
				printf(" >>>>>>>>>>>>>>>>>>>>>> DEC  MFX_WRN_DEVICE_BUSY  <<<<<<<<<<<<<<<<<<<<<<< \n");
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
    			//pmfxWorkSurfaceDEC = NULL;
    			break;
    		case MFX_WRN_VIDEO_PARAM_CHANGED:
    			//TODO
    			// Drain all frames by using NULL bitstream
    			// call MFXVideoDECODE_GetVideoParam() realloc surface pool
    			// call MFXVideoDECODE_Close() to restart
    			break;
			case MFX_ERR_NONE:
				break;
    		default:
    			//other error, we cannot handle
				fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "DecodeFrameAsync() return err %d\n" ANSI_COLOR_RESET, sts);
				exit(1);
				assert(0);
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
					if(phddlSurfaceDEC->Data.Corrupted)
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
    	if(m_debug == Debug::yes || m_debug == Debug::dec)
    	{
			printf("%s:%d, %d,%d, id:dec_%d,vpp_%d, Outsurface:%d, Locked:%d, FrameOrder:0x%X, Timestamp:%llu index:%d\n",
					__FILENAME__,__LINE__, bRunningDEC, bOutReadyDEC, dec_id, vpp_id,
					phddlSurfaceDEC?spDEC.surfaceID(phddlSurfaceDEC):-1,
					phddlSurfaceDEC?phddlSurfaceDEC->Data.Locked:-1,
					phddlSurfaceDEC?phddlSurfaceDEC->Data.FrameOrder:-1,
					phddlSurfaceDEC?phddlSurfaceDEC->Data.TimeStamp:-1,
					phddlSurfaceDEC?phddlSurfaceDEC->index():-1);
    	}

    	if(phddlSurfaceDEC) {
    		phddlSurfaceDEC->m_FrameNumber = dec_id;
    		//spDEC.debug();
    		spDEC.reserve(phddlSurfaceDEC, drop_on_overflow);
    		dec_id ++;
    	}

    	//2nd stage VPP
    	if(bOutReadyDEC || !bRunningDEC){
    		//
    		mfxFrameSurface1* pmfxOutSurfaceVPP = spVPP.getfree();
			if(pmfxOutSurfaceVPP == NULL){
				fprintf(stderr, "%s:%d spVPP.getfree() return NULL\n", __FILENAME__, __LINE__);
				goto DECODE_LOOPEND;
			}

            // until some meaningful result is got
			mfxFrameSurface1* pmfxSurfaceOut = NULL;
            do
            {
                // Process a frame asychronously (returns immediately)
                sts = mfxVPP.RunFrameVPPAsync(phddlSurfaceDEC, pmfxOutSurfaceVPP, NULL, &syncpV);

				if (MFX_WRN_DEVICE_BUSY == sts) {
					MSDK_SLEEP(1);  // wait if device is busy
					printf(" >>>>>>>>>>>>>>>>>>>>>> VPP MFX_WRN_DEVICE_BUSY  <<<<<<<<<<<<<<<<<<<<<<< \n");
				}
                //if(MFX_ERR_NONE != sts) printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %d, %p\n", sts, syncpV);

            }while(MFX_ERR_NONE < sts && !syncpV);

            phddlSurfaceVPP = static_cast<surface1 *>(pmfxOutSurfaceVPP);

            if(m_debug == Debug::yes)
            	printf("%s:%d, [%d,%d] vpp_id %d, sts:%d syncpV:%p\n",__FILENAME__,__LINE__, bRunningDEC, bOutReadyDEC, vpp_id, sts, syncpV);

            switch(sts)
            {
            case MFX_ERR_MORE_DATA:
            	if(phddlSurfaceDEC == NULL){
            		//fprintf(stderr,"WARNING: Decoder is done while VPP still requires more data\n");
            		bRunningVPP = false;
            	}
            	break;
            case MFX_ERR_MORE_SURFACE:
                // Not relevant for the illustrated workload! Therefore not handled.
                // Relevant for cases when VPP produces more frames at output than consumes at input.
            	// E.g. framerate conversion 30 fps -> 60 fps
				fprintf(stderr,"%s:%d Cannot handle MFX_ERR_MORE_SURFACE\n", __FILENAME__, __LINE__);
				goto DECODE_LOOPEND;
            	break;
            default:
            	MSDK_BREAK_ON_ERROR(sts);
            }

            if (MFX_ERR_NONE <= sts && syncpV){
            	// ignore warnings if output is available
   				// Synchronize. Wait until decoded frame is ready
    			if (MFX_ERR_NONE == (sts=session.SyncOperation(syncpV, 60000))) {

    				//reserve VPP output surface & find corresponding decode output
    				spVPP.reserve(phddlSurfaceVPP, drop_on_overflow);
    				phddlSurfaceVPP->m_FrameNumber = vpp_id;

    				surface1 * phddlSurfaceVPPDEC = spDEC.find(phddlSurfaceVPP->Data.FrameOrder);
    				if(phddlSurfaceVPPDEC)
    					spDEC.reserve(phddlSurfaceVPPDEC, drop_on_overflow);
    				else
    				{
    					fprintf(stderr,"%s:%d Cannot find decoder output by FrameOrder\n", __FILENAME__, __LINE__);
    					goto DECODE_LOOPEND;
    				}

    				if(m_debug == Debug::out || m_debug == Debug::yes){
    					printf("%s:%d, [%d,%d], id:%d,%d, DEC%lu@%d(%dx%d %c%c%c%c locked_%d reserved_%d forder_0x%X),VPP%lu@%d(%dx%d %c%c%c%c locked_%d reserved_%d forder_0x%X)\n",
    							__FILENAME__,__LINE__, bRunningDEC, bOutReadyDEC, dec_id, vpp_id,
								phddlSurfaceVPPDEC->m_FrameNumber,phddlSurfaceVPPDEC->index(),
								phddlSurfaceVPPDEC->Info.Width, phddlSurfaceVPPDEC->Info.Height,
								((char*)(&phddlSurfaceVPPDEC->Info.FourCC))[0],
								((char*)(&phddlSurfaceVPPDEC->Info.FourCC))[1],
								((char*)(&phddlSurfaceVPPDEC->Info.FourCC))[2],
								((char*)(&phddlSurfaceVPPDEC->Info.FourCC))[3],
								phddlSurfaceVPPDEC->Data.Locked, phddlSurfaceVPPDEC->is_reserved(),	phddlSurfaceVPPDEC->Data.FrameOrder,
								phddlSurfaceVPP->m_FrameNumber,phddlSurfaceVPP->index(),
								phddlSurfaceVPP->Info.Width, phddlSurfaceVPP->Info.Height,
								((char*)(&phddlSurfaceVPP->Info.FourCC))[0],
								((char*)(&phddlSurfaceVPP->Info.FourCC))[1],
								((char*)(&phddlSurfaceVPP->Info.FourCC))[2],
								((char*)(&phddlSurfaceVPP->Info.FourCC))[3],
								phddlSurfaceVPP->Data.Locked, phddlSurfaceVPP->is_reserved(),	phddlSurfaceVPP->Data.FrameOrder);
    				}

    				//setup deleter as unreserve() so it can be re-cycled
    				//note the deleter will be called from user thread context, so it must be multithread-safe
					std::shared_ptr<surface1> o1(phddlSurfaceVPPDEC, [this](surface1*p) {spDEC.unreserve(p); });
					std::shared_ptr<surface1> o2(phddlSurfaceVPP, [this](surface1*p) {spVPP.unreserve(p); });

					//only output both reserved frames(or they will got unreserved automatically by shared_ptr)
					bool bEnqueueOK = false;

					if (o1->is_reserved() && o2->is_reserved())
					{
						bEnqueueOK = m_outputs.put(Output(o1, o2), drop_on_overflow);
					}

					if (!bEnqueueOK)
					{
						dropped_cnt++;
						//printf(ANSI_BOLD ANSI_COLOR_CYAN "thread %d: Drop on overflow %d!\n" ANSI_COLOR_RESET,std::this_thread::get_id(), dropped_cnt);
						//don't need un-reserve because of shared_ptr
					}

    				vpp_id++;
    			}else{
    				if(m_debug == Debug::out || m_debug == Debug::yes)
    					fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "%s:%d SyncOperation() failed with %d\n" ANSI_COLOR_RESET, __FILENAME__,__LINE__, sts);
    			}
            }
    	}
    }

    //close output pipe/queue
    m_outputs.close();
	//wait user call stop()
	while(!m_stop);

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
