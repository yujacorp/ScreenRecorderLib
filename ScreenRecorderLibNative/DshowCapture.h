#pragma once
#include <Wmcodecdsp.h>
#include <Windows.h>
#include <mfidl.h> 
#include <Mfapi.h> 
#include <Mfreadwrite.h>
#include <propvarutil.h>
#include <Shlwapi.h>
#include <atlbase.h>
#include "CommonTypes.h"
#include "HighresTimer.h"
#include "LogMediaType.h"
#include "CaptureBase.h"
#include "TextureManager.h"

#ifndef __CAPTURE_MEMBER_FUNCTION_CALLBACK_BASE_H
#define CAPTURE_MEMBER_FUNCTION_CALLBACK_BASE_H
#include "CaptureMemberFunctionCallbackBase.h"
#endif //CAPTURE_MEMBER_FUNCTION_CALLBACK_BASE_H

class DshowCapture : public CaptureBase
{
public:
	virtual void Close();
	DshowCapture();
	virtual ~DshowCapture();
	virtual inline std::wstring Name() override { return L"CameraCapture"; };
	virtual HRESULT StartCapture(_In_ RECORDING_SOURCE_BASE &recordingSource) override;
	virtual HRESULT GetNativeSize(_In_ RECORDING_SOURCE_BASE &recordingSource, _Out_ SIZE *nativeMediaSize) override;
	virtual HRESULT AcquireNextFrame(_In_ DWORD timeoutMillis, _Outptr_opt_ ID3D11Texture2D **ppFrame) override;
	virtual HRESULT Initialize(_In_ ID3D11DeviceContext *pDeviceContext, _In_ ID3D11Device *pDevice) override;
	virtual HRESULT WriteNextFrameToSharedSurface(_In_ DWORD timeoutMillis, _Inout_ ID3D11Texture2D *pSharedSurf, INT offsetX, INT offsetY, _In_ RECT destinationRect) override;
	inline virtual HRESULT GetMouse(_Inout_ PTR_INFO *pPtrInfo, _In_ RECT frameCoordinates, _In_ int offsetX, _In_ int offsetY) override {
		return S_FALSE;
	}

	// the class must implement the methods from IUnknown 
	STDMETHODIMP QueryInterface(REFIID iid, void **ppv);
	STDMETHODIMP_(ULONG) AddRef();
	STDMETHODIMP_(ULONG) Release();

	typedef void(__stdcall *LPFN_CaptureCallback)(DWORD dwSize, BYTE *pbData);

	virtual void __stdcall CaptureCallback(DWORD dwSize, BYTE *pbData);

protected:
	virtual HRESULT DshowCapture::ResizeFrameBuffer(UINT bufferSize);
	
	CRITICAL_SECTION m_CriticalSection;
	inline IMFDXGIDeviceManager *GetDeviceManager() { return m_DeviceManager; }
private:
	long m_ReferenceCount;
	HANDLE m_NewFrameEvent;
	HANDLE m_StopCaptureEvent;
	LARGE_INTEGER m_LastSampleReceivedTimeStamp;
	LARGE_INTEGER m_LastGrabTimeStamp;
	IMFMediaBuffer *m_Sample_d;
	ID3D11Device *m_Device;
	ID3D11DeviceContext *m_DeviceContext;
	HighresTimer *m_FramerateTimer;
	//IMFMediaType *m_OutputMediaType;
	//IMFMediaType *m_InputMediaType;
	//IMFTransform *m_MediaTransform;
	std::unique_ptr<TextureManager> m_TextureManager;
	CComPtr<IMFDXGIDeviceManager> m_DeviceManager;
	RECORDING_SOURCE_BASE *m_RecordingSource;
	UINT m_ResetToken;
	UINT m_BufferSize;
	_Field_size_bytes_(m_BufferSize) BYTE *m_PtrFrameBuffer;
	LONG m_Stride;
	SIZE m_FrameSize;
	double m_FrameRate;
	CaptureMemberFunctionCallback *cmfbr;
	int d_Width;
	int d_Height;
	int d_Stride;
	IUnknown *d_CameraPtr;
	int pipelineIndex;
	std::wstring friendlyName;
};

