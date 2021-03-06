cmake_minimum_required(VERSION 2.8)
PROJECT(mediaSDK_wrapper)

MESSAGE(STATUS "operation system is ${CMAKE_SYSTEM_NAME}")  

if ( UNIX )
	MESSAGE(STATUS "current platform: Linux ")  
	
	set (CMAKE_CXX_STANDARD 11)
	
	set(SRC common/common_vaapi.cpp
			common/cmd_options.cpp
			common/common_utils.cpp
			common/surface_pool.cpp
			common/media_pipeline.cpp
			common/videoframe_allocator.cpp
			)
			
	set(LIB mfx va va-drm pthread rt dl OpenCL)
	set(INCDIR common
	           /usr/local/include
			   $ENV{MFX_HOME}include)
			   
	# this is embedded version of libva 
	set(RPATH "/home/hddls/hdd/tools/MSS/Unified_3D_MR3.1/intel-linux-ufo-yocto_bxt-16.7.3-64751-ubit-64bit/usr/lib/")
	
	set(LIBDIR $ENV{MFX_HOME}/lib/lin_x64 
				${RPATH} 
				/usr/lib/x86_64-linux-gnu/)
	
	# -rdynamic will cause problem, remove it
	SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
	SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
	
elseif ( WIN32 )
	MESSAGE(STATUS "current platform: Windows")  
	set(SRC common/common_directx.cpp
			common/cmd_options.cpp
			common/common_utils.cpp
			common/surface_pool.cpp
			common/media_pipeline.cpp
			common/videoframe_allocator.cpp)
			
	set(LIB libmfx_vs2017.lib DXGI.lib D3D9.lib dxva2.lib)
	set(INCDIR common $ENV{INTELMEDIASDKROOT}include )
	set(LIBDIR $ENV{INTELMEDIASDKROOT}lib\\x64 )
	add_definitions(-DDX9_D3D=1)
else ()
    message( FATAL_ERROR "Only UNIX & WIN32 are supported" )
endif ()


MESSAGE(STATUS "INCDIR=" ${INCDIR})
MESSAGE(STATUS "LIBDIR=" ${LIBDIR})

link_directories(${LIBDIR})
include_directories(${INCDIR})
LINK_LIBRARIES(${LIB})
ADD_EXECUTABLE(test_decode_vpp ${SRC} test_decode_vpp.cpp)

