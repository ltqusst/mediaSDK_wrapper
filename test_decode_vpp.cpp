/*****************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or
nondisclosure agreement with Intel Corporation and may not be copied
or disclosed except in accordance with the terms of that agreement.
Copyright(c) 2005-2014 Intel Corporation. All Rights Reserved.

*****************************************************************************/

#include "common_utils.h"
#include "cmd_options.h"

#include "media_pipeline.h"

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
#include <deque>
#include <condition_variable>
#include <chrono>


//======================================================================================

//======================================================================================
#if 0
//===========================================================
#include <CL/cl.h>
#include <va/va.h>
#include <common_vaapi.h>

enum cl_va_api_device_source_intel {
	CL_VA_API_DISPLAY_INTEL=0x4094
};

enum cl_va_api_device_set_intel {
    CL_PREFERRED_DEVICES_FOR_VA_API_INTEL = 0x4095,
    CL_ALL_DEVICES_FOR_VA_API_INTEL       = 0x4096
};
#define CL_CONTEXT_VA_API_DISPLAY_INTEL   0x4097

class ContextOpenCL
{
public:
	cl_platform_id 	m_platform_id;
	cl_device_id 	m_device;
	cl_context 		m_context;
	static ContextOpenCL & instance(){
		static std::once_flag oc;
		static ContextOpenCL cl;
		std::call_once(oc, []{
				cl.init();
		});
		return cl;
	}

	typedef cl_int (*FclGetDeviceIDsFromVA_APIMediaAdapterINTEL)   (
        cl_platform_id platform,
        cl_va_api_device_source_intel media_adapter_type  ,
        void *media_adapter,
        cl_va_api_device_set_intel media_adapter_set,
        cl_uint num_entries,
        cl_device_id *devices,
        cl_uint *num_devices);

	typedef cl_mem (*FclCreateFromVA_APIMediaSurfaceINTEL)(
        cl_context context,
        cl_mem_flags flags,
        VASurfaceID *surface ,
        cl_uint plane,
        cl_int *errcode_ret);

	typedef cl_int (*FclEnqueueAcquireVA_APIMediaSurfacesINTEL)(
        cl_command_queue command_queue,
        cl_uint num_objects,
        const cl_mem *mem_objects,
        cl_uint num_events_in_wait_list,
        const cl_event *event_wait_list,
        cl_event *event);

	typedef cl_int (*FclEnqueueReleaseVA_APIMediaSurfacesINTEL)(
        cl_command_queue command_queue,
        cl_uint num_objects,
        const cl_mem *mem_objects,
        cl_uint num_events_in_wait_list,
        const cl_event *event_wait_list,
        cl_event *event);

	FclGetDeviceIDsFromVA_APIMediaAdapterINTEL  clGetDeviceIDsFromVA_APIMediaAdapterINTEL;
	FclCreateFromVA_APIMediaSurfaceINTEL		clCreateFromVA_APIMediaSurfaceINTEL;
	FclEnqueueAcquireVA_APIMediaSurfacesINTEL   clEnqueueAcquireVA_APIMediaSurfacesINTEL;
	FclEnqueueReleaseVA_APIMediaSurfacesINTEL   clEnqueueReleaseVA_APIMediaSurfacesINTEL;

	int build_program( const char * filename,
			           cl_program *ptr_program)
	{
		// Compile the kernel
		const char * kernelstring = load_src(filename);
		if(kernelstring == NULL){
			fprintf(stderr, ">>> clutl_build_kernel: load_src(%s) error\n", filename);
			return 1;
		}

		cl_program program = clCreateProgramWithSource(m_context, 1, &kernelstring, NULL, NULL);
		clBuildProgram(program, 0, NULL, "", NULL, NULL);

		free((void*)kernelstring);

		// Check for compilation errors
		size_t logSize;
		clGetProgramBuildInfo(program, m_device, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
		char* messages = (char*)malloc((1+logSize)*sizeof(char));
		clGetProgramBuildInfo(program, m_device, CL_PROGRAM_BUILD_LOG, logSize, messages, NULL);
		messages[logSize] = '\0';
		if (logSize > 10) { fprintf(stderr, ">>> Compiler message: %s\n", messages); }
		free(messages);

		*ptr_program = program;
		return 0;
	}

private:


	ContextOpenCL():
		m_context(NULL){}
#define LINUX
#ifdef LINUX
	bool init(){
		VADisplay display = VAHandle::get();
    	cl_uint 		ret_num_platforms;
    	cl_platform_id 	platform_id[32]={0};
    	cl_int status;
	    /* Get Platform and Device Info */
    	status = clGetPlatformIDs(sizeof(platform_id)/sizeof(platform_id[0]),
	    				platform_id,
						&ret_num_platforms);

    	assert(status == CL_SUCCESS || (printf("@%s:%d\n", __FILE__,__LINE__),0));

	    m_platform_id = NULL;
	    for(int i=0;i<ret_num_platforms;i++){
	    	clGetDeviceIDsFromVA_APIMediaAdapterINTEL = (FclGetDeviceIDsFromVA_APIMediaAdapterINTEL)
	    			clGetExtensionFunctionAddressForPlatform(platform_id[i], "clGetDeviceIDsFromVA_APIMediaAdapterINTEL");
	    	clCreateFromVA_APIMediaSurfaceINTEL = (FclCreateFromVA_APIMediaSurfaceINTEL)
	    			clGetExtensionFunctionAddressForPlatform(platform_id[i], "clCreateFromVA_APIMediaSurfaceINTEL");
	    	clEnqueueAcquireVA_APIMediaSurfacesINTEL = (FclEnqueueAcquireVA_APIMediaSurfacesINTEL)
	    			clGetExtensionFunctionAddressForPlatform(platform_id[i], "clEnqueueAcquireVA_APIMediaSurfacesINTEL");
	    	clEnqueueReleaseVA_APIMediaSurfacesINTEL = (FclEnqueueReleaseVA_APIMediaSurfacesINTEL)
	    			clGetExtensionFunctionAddressForPlatform(platform_id[i], "clEnqueueReleaseVA_APIMediaSurfacesINTEL");

	    	if( (void*)clGetDeviceIDsFromVA_APIMediaAdapterINTEL == NULL ||
	    		(void*)clCreateFromVA_APIMediaSurfaceINTEL == NULL ||
				(void*)clEnqueueAcquireVA_APIMediaSurfacesINTEL == NULL ||
				(void*)clEnqueueReleaseVA_APIMediaSurfacesINTEL == NULL)
	    	{
	    		continue;
	    	}
		    //
	        // Query device list

	        cl_uint numDevices = 0;

	        status = clGetDeviceIDsFromVA_APIMediaAdapterINTEL(platform_id[i], CL_VA_API_DISPLAY_INTEL, display,
	                                                           CL_PREFERRED_DEVICES_FOR_VA_API_INTEL, 0, NULL, &numDevices);
	        if ((status != CL_SUCCESS) || (numDevices == 0))
	            continue;

	        numDevices = 1; // initializeContextFromHandle() expects only 1 device
	        status = clGetDeviceIDsFromVA_APIMediaAdapterINTEL(platform_id[i], CL_VA_API_DISPLAY_INTEL, display,
	                                                           CL_PREFERRED_DEVICES_FOR_VA_API_INTEL, numDevices, &m_device, NULL);
	        if (status != CL_SUCCESS)
	            continue;

	        // Creating CL-VA media sharing OpenCL context
			cl_context_properties props[] = {
				CL_CONTEXT_VA_API_DISPLAY_INTEL, (cl_context_properties) display,
				CL_CONTEXT_INTEROP_USER_SYNC, CL_FALSE, // no explicit sync required
				0
			};

			m_context = clCreateContext(props, numDevices, &m_device, NULL, NULL, &status);
			if (status != CL_SUCCESS)
			{
				clReleaseDevice(m_device);
			}
			else
			{
				m_platform_id = platform_id[i];
				break;
			}
	    }

	    if(m_platform_id == NULL) return false;
	}
#endif

	char * load_src(const char *fileName)
	{
	#define MAX_SOURCE_SIZE (0x100000)
		char * source_str = NULL;
		FILE *fp = fopen(fileName, "r");
		size_t source_size = 0;
	    if (!fp) {
			fprintf(stderr, "Failed to load kernel.\n");
			return NULL;
	    }
	    source_str = (char*)malloc(MAX_SOURCE_SIZE);
	    source_size = fread(source_str, 1, MAX_SOURCE_SIZE, fp);
	    source_str[source_size] = 0;
	    fclose(fp);
	    return source_str;
	}

};

#endif

void decode(const char * bsfile, mfxIMPL impl, bool drop_on_overflow, const char * ofile)
{
    MediaDecoder m(8);
    FILE* fSink = NULL;
    if(ofile){
    	fSink = fopen(ofile,"wb");
    }

    auto t_start = std::chrono::high_resolution_clock::now();

    m.start(bsfile, impl, drop_on_overflow);

	int nFrame;
    for(nFrame=0; nFrame < 1000;nFrame++){
    	MediaDecoder::Output out;
    	if(!m.get(out)) break;

    	//usleep(2000);
		while (fSink) {
			mfxStatus sts = out.second->lock();
			if(sts != MFX_ERR_NONE)
				fprintf(stderr, ANSI_BOLD ANSI_COLOR_RED "========================Lock() failed %d\n", sts);

			mfxPrintFrameInfo(out.second->Info); printf("\n");

			sts = WriteRawFrame(static_cast<mfxFrameSurface1*>(out.second.get()), fSink);
			MSDK_BREAK_ON_ERROR(sts);

			MSDK_BREAK_ON_ERROR(out.second->unlock());
			printf("%sFrame number (DEC, VPP): %d, %d \n" ANSI_COLOR_RESET,
				out.first->m_FrameNumber == out.second->m_FrameNumber?ANSI_COLOR_RESET:ANSI_COLOR_RED,
				out.first->m_FrameNumber,
				out.second->m_FrameNumber);
			break;
		}
		//the surface in out will automatically returned
    }

    m.stop();

	auto t_end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> diff = t_end - t_start;

    double fps = ((double)nFrame / diff.count());
    printf("\nTotal Frames: %d, Execution time: %3.2f s (%3.2f fps)\n", nFrame, diff.count(), fps);

    if (fSink) fclose(fSink);
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

    mfxU32 nFrame = 0;

    //ContextOpenCL &o = ContextOpenCL::instance();

    bool drop_on_overflow = options.values.AutoDropFrames;

	std::thread * pth[32] = { 0 };

#define cntof(x) sizeof(x)/sizeof(x[0])
    for(int t=0;t<options.values.Channels;t++)
    {
    	pth[t] = new std::thread(decode, options.values.SourceName, options.values.impl, drop_on_overflow,
			t== options.values.Channels-1?(bEnableOutput?options.values.SinkName:NULL):NULL);
    }

    for(int t=0;t<cntof(pth);t++)
    {
		if(pth[t])
    		pth[t]->join();
    }
}
