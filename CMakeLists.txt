cmake_minimum_required(VERSION 2.8)
PROJECT(simple_decode_vpp_vmem)

MESSAGE(STATUS "operation system is ${CMAKE_SYSTEM_NAME}")  

if ( Unix )
	MESSAGE(STATUS "current platform: Linux ")  
	set(SRC simple_decode_vpp_vmem.cpp
			common/cmd_options.cpp
			common/common_utils.cpp
			common/common_utils_linux.cpp
			common/common_vaapi.cpp)
			
	set(LIB lmfx lva lva-drm lpthread lrt ldl)
	set(INCDIR ./common
	           /usr/local/include
			   $(MFX_HOME)/include)
	set(LIBDIR .)
elseif ( WIN32 )
	MESSAGE(STATUS "current platform: Windows")  
	set(SRC simple_decode_vpp_vmem.cpp
			common/cmd_options.cpp
			common/common_directx11.cpp
			common/common_utils.cpp
			common/common_utils_windows.cpp)
			
	set(LIB libmfx_vs2015_d.lib DXGI.lib D3D11.lib)
	set(INCDIR common $ENV{INTELMEDIASDKROOT}include )
	set(LIBDIR $ENV{INTELMEDIASDKROOT}lib\\x64 )
endif ()

MESSAGE(STATUS "INCDIR=" ${INCDIR})
MESSAGE(STATUS "LIBDIR=" ${LIBDIR})

link_directories(${LIBDIR})
include_directories(${INCDIR})
LINK_LIBRARIES(${LIB})

ADD_EXECUTABLE(${PROJECT_NAME} ${SRC})
