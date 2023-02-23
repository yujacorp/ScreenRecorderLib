#pragma once

#ifdef  VIDEOCAMLIB_EXPORTS 
/*Enabled as "export" while compiling the dll project*/
#define DLLEXPORT __declspec(dllexport)  
#include "stdafx.h"
#else
/*Enabled as "import" in the Client side for using already created dll file*/
#define DLLEXPORT __declspec(dllimport)  
#endif

#include <dshow.h>
#include "qedit.h"
#include <strsafe.h>

typedef void(__stdcall* PFN_CaptureCallback)(DWORD dwSize, BYTE* pbData);

HRESULT ConfigureSampleGrabber(IBaseFilter* pIBaseFilter, PFN_CaptureCallback pfnCallback);
HRESULT ConfigureFileSampleGrabber(IBaseFilter* pIBaseFilter, PFN_CaptureCallback pfnCallback);
IPin* GetPin(IBaseFilter* pFilter, PIN_DIRECTION pinDir, int index);
void CleanupCameraInfo();

extern "C"
{
	DLLEXPORT DWORD APIENTRY StopCamera();
	DLLEXPORT DWORD APIENTRY RefreshCameraList(int* nCount);
	DLLEXPORT DWORD APIENTRY GetIndexOfSymbolicName(int* indexOfSymbolicName, BSTR symbolicName);
	DLLEXPORT DWORD APIENTRY GetCameraDetails(int index, IUnknown** ppUnk, BSTR* pbstrName);
	DLLEXPORT DWORD APIENTRY DisplayCameraPropertiesDialog(IUnknown* pUnk, HWND hwnd);
	DLLEXPORT DWORD APIENTRY StartCamera(IUnknown* pUnk, PFN_CaptureCallback pfnCallback, int minwidth, int minheight, int* pnWidth, int* pnHeight, int* pnStride, int* pipelineIndexOut);
	DLLEXPORT DWORD APIENTRY StartVideoFile(LPCWSTR wFileName, PFN_CaptureCallback pfnCallback, int* pnWidth, int* pnHeight, int* pnStride);
	DLLEXPORT DWORD APIENTRY StopSpecificCamera(int pipelineIndex);
	DLLEXPORT DWORD APIENTRY VideoCamLibInitialize();
	DLLEXPORT DWORD APIENTRY VideoCamLibCleanup();
}