#include "DshowCapture.h"
#include <Mferror.h>
#include "Cleanup.h"
#include "VideoCamLib.h"
#include "CaptureMemberFunctionCallbackBase.h"

using namespace std;

DshowCapture::DshowCapture() :
	m_Sample_d{ nullptr },
	m_Device(nullptr),
	m_DeviceContext(nullptr),
	m_LastSampleReceivedTimeStamp{ 0 },
	m_LastGrabTimeStamp{ 0 },
	m_ReferenceCount(1),
	m_Stride(0),
	m_FrameRate(0),
	m_FrameSize{},
	m_FramerateTimer(nullptr),
	m_NewFrameEvent(nullptr),
	m_StopCaptureEvent(nullptr),
	m_TextureManager(nullptr),
	m_BufferSize(0),
	m_PtrFrameBuffer(nullptr),
	m_RecordingSource(nullptr),
	m_DeviceManager(nullptr),
	m_ResetToken(0),
	cmfbr(nullptr),
	friendlyName{}
{
	InitializeCriticalSection(&m_CriticalSection);
	m_NewFrameEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	m_StopCaptureEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	
	d_Width = 0;
	d_Height = 0;
	d_Stride = 0;
	d_CameraPtr = nullptr;
	pipelineIndex = -1;
	cmfbr = new CaptureMemberFunctionCallback(this, &DshowCapture::CaptureCallback);
}
DshowCapture::~DshowCapture()
{
	Close();
	EnterCriticalSection(&m_CriticalSection);
	SafeRelease(&m_Device);
	SafeRelease(&m_DeviceContext);

	delete m_FramerateTimer;
	CloseHandle(m_NewFrameEvent);
	CloseHandle(m_StopCaptureEvent);
	LeaveCriticalSection(&m_CriticalSection);
	DeleteCriticalSection(&m_CriticalSection);

	delete[] m_PtrFrameBuffer;
	m_PtrFrameBuffer = nullptr;
}

HRESULT DshowCapture::StartCapture(_In_ RECORDING_SOURCE_BASE &recordingSource)
{
	HRESULT hr = S_OK;
	EnterCriticalSection(&m_CriticalSection);
	LeaveCriticalSectionOnExit leaveCriticalSection(&m_CriticalSection, L"StartCapture");
	m_RecordingSource = &recordingSource;
	ResetEvent(m_StopCaptureEvent);
	if (recordingSource.Type == RecordingSourceType::CameraCapture)
	{
		int vcCount = -1;
		int index = -1;
		BSTR name = nullptr;
		IUnknown *ptr = nullptr;
		hr = RefreshCameraList(&vcCount);
		wstring ffmpegName = wstring(recordingSource.SourcePath).replace(recordingSource.SourcePath.find('_'), 1, 1, ':');
		ffmpegName.replace(ffmpegName.find('_'), 1, 1, ':');
		BSTR symbolicName = SysAllocStringLen(ffmpegName.data(), ffmpegName.size());
		hr = GetIndexOfSymbolicName(&index, symbolicName);
		SysFreeString(symbolicName);

		if (index < 0) 
		{
			return E_UNEXPECTED;
		}

		hr = GetCameraDetails(index, &ptr, &name);
		friendlyName = _bstr_t(name, true);

		SysFreeString(name); //apparently transferred owndership?
		SIZE size;
		if (recordingSource.OutputSize.has_value()) 
		{
			size = recordingSource.OutputSize.value();
		}
		else 
		{
			size = { 0 };
			size.cx = 640;
			size.cy = 480;
		}

		hr = StartCamera(ptr, *cmfbr, size.cx, size.cy, &d_Width, &d_Height, &d_Stride, &pipelineIndex);
	}
	return hr;
}

void __stdcall DshowCapture::CaptureCallback(DWORD dwSize, BYTE *pbData)
{
	HRESULT hr = S_OK;

	if (m_RecordingSource == nullptr || m_RecordingSource->Type != RecordingSourceType::CameraCapture)
	{
		return;
	}

	bool cs = TryEnterCriticalSection(&m_CriticalSection);

	if (!cs) 
	{
		return;
	}

	if (!friendlyName.empty() && !m_FramerateTimer)
	{
		LOG_INFO("CaptureCallback: Name of Device: %ls", friendlyName);
	}

	SafeRelease(&m_Sample_d);
	//Store the converted media buffer
	IMFMediaBuffer *mediaBuffer = NULL;
	hr = MFCreateMemoryBuffer(dwSize, &mediaBuffer);

	if (hr != S_OK) 
	{
		LOG_ERROR("CaptureCallback: Error with creating Memory buffer code = %d", hr);
		return;
	}

	uint8_t *outData = nullptr;
	DWORD outLen = -1;

	//all d_Stride is negative to flip the image the correct way
	int bytesPerPixel = abs(-d_Stride) / d_Width;

	mediaBuffer->Lock(&outData, nullptr, &outLen);
	hr = MFCopyImage(
					outData,       // Destination buffer.
					abs(d_Stride),                    // Destination stride. We use the absolute value to flip bitmaps with negative stride. 
					-d_Stride > 0 ? pbData : pbData + (d_Height - 1) * abs(-d_Stride), // First row in source image with positive stride, or the last row with negative stride.
					-d_Stride,//m_Stride,						  // Source stride.
					d_Width * bytesPerPixel,//bytesPerPixel * m_FrameSize.cx,	      // Image width in bytes.
					d_Height//m_FrameSize.cy						  // Image height in pixels.
	);
	mediaBuffer->SetCurrentLength(dwSize);
	mediaBuffer->Unlock();

	m_Sample_d = mediaBuffer;
	QueryPerformanceCounter(&m_LastSampleReceivedTimeStamp);
	SetEvent(m_NewFrameEvent);

	if (!m_FramerateTimer) {
		m_FramerateTimer = new HighresTimer();
		m_FramerateTimer->StartRecurringTimer((INT64)round(m_FrameRate));
	}
	if (m_FrameRate > 0) {
		auto t1 = std::chrono::high_resolution_clock::now();
		auto sleepTime = m_FramerateTimer->GetMillisUntilNextTick();
		MeasureExecutionTime measureNextTick(L"OnReadSample scheduled delay");
		hr = m_FramerateTimer->WaitForNextTick();
		if (SUCCEEDED(hr)) {
			auto t2 = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> ms_double = t2 - t1;
			double diff = ms_double.count() - sleepTime;
			measureNextTick.SetName(string_format(L"OnReadSample scheduled delay for %.2f ms for next frame. Actual delay differed by: %.2f ms, with a total delay of", sleepTime, diff));
		}
	}
	LeaveCriticalSection(&m_CriticalSection);

	return;
}

HRESULT DshowCapture::GetNativeSize(_In_ RECORDING_SOURCE_BASE &recordingSource, _Out_ SIZE *nativeMediaSize)
{
	MeasureExecutionTime measure(L"SourceReaderBase GetNativeSize");
	HRESULT hr = S_OK;
	//EnterCriticalSection(&m_CriticalSection);
	//LeaveCriticalSectionOnExit leaveCriticalSection(&m_CriticalSection, L"StartCapture");
	//int vcCount = -1;
	//int index = -1;
	//wstring ffmpegName = wstring(recordingSource.SourcePath).replace(recordingSource.SourcePath.find('_'), 1, 1, ':');
	//ffmpegName.replace(ffmpegName.find('_'), 1, 1, ':');
	//BSTR symbolicName = SysAllocStringLen(ffmpegName.data(), ffmpegName.size());
	//hr = RefreshCameraList(&vcCount);
	//hr = GetIndexOfSymbolicName(&index, symbolicName);
	//SysFreeString(symbolicName);
	//if (index < 0)
	//{
	//	*nativeMediaSize = SIZE{ 640, 480 };
	//	return E_UNEXPECTED;
	//}

	////todo: start camera capture and properly populate

	//BSTR name = nullptr;
	//IUnknown *ptr = nullptr;
	//hr = GetCameraDetails(index, &ptr, &name);
	//SysFreeString(name);

	SIZE size;
	if (recordingSource.OutputSize.has_value())
	{
		size = recordingSource.OutputSize.value();
	}
	else
	{
		size = { 0 };
		size.cx = 640;
		size.cy = 480;
	}

	//hr = StartCamera(ptr, *cmfbr, size.cx, size.cy, &d_Width, &d_Height, &d_Stride, &pipelineIndex);
	//if (pipelineIndex >= 0 && pipelineIndex < 6)
	//{
	//	StopSpecificCamera(pipelineIndex);
	//	pipelineIndex = -1;
	//}

	//*nativeMediaSize = SIZE{ (LONG)d_Width,(LONG)d_Height };
	*nativeMediaSize = SIZE{ (LONG)size.cx,(LONG)size.cy };
	return S_OK;
}

void DshowCapture::Close()
{
	EnterCriticalSection(&m_CriticalSection);
	SafeRelease(&m_Sample_d);
	if (m_FramerateTimer) {
		LOG_DEBUG("Stopping source reader sync timer");
		m_FramerateTimer->StopTimer(true);
	}

	if (cmfbr) //this needs to be before StopSpecificCamera to prevent race condition
	{
		delete cmfbr;
	}

	if (pipelineIndex >= 0 && pipelineIndex < 6) 
	{
		StopSpecificCamera(pipelineIndex);
	}
	
	SetEvent(m_StopCaptureEvent);

	LeaveCriticalSection(&m_CriticalSection);

	//SafeRelease(&m_InputMediaType);
	//SafeRelease(&m_MediaTransform);
	LOG_DEBUG("Closed source reader");

	SafeRelease(&m_Sample_d);

	VideoCamLibCleanup(); //not needed
}

HRESULT DshowCapture::AcquireNextFrame(_In_ DWORD timeoutMillis, _Outptr_opt_ ID3D11Texture2D **ppFrame)
{
	DWORD result = WAIT_OBJECT_0;

	if (m_LastGrabTimeStamp.QuadPart >= m_LastSampleReceivedTimeStamp.QuadPart) {
		result = WaitForSingleObject(m_NewFrameEvent, timeoutMillis);
	}
	HRESULT hr = S_OK;
	if (result == WAIT_OBJECT_0) {
		//Only create frame if the caller accepts one.
		if (ppFrame) {
			EnterCriticalSection(&m_CriticalSection);
			LeaveCriticalSectionOnExit leaveCriticalSection(&m_CriticalSection, L"GetFrameBuffer");

			BYTE *data_d = nullptr;
			DWORD len_d = -1;
			if (m_Sample_d == nullptr) {
				hr = E_FAIL;
			}
			else 
			{
				if (m_Sample_d != nullptr)
					hr = m_Sample_d->Lock(&data_d, NULL, &len_d);
			}
			if (FAILED(hr))
			{
				delete[] m_PtrFrameBuffer;
				m_PtrFrameBuffer = nullptr;
				return hr;
			}
			if (SUCCEEDED(hr)) {
				if (m_Sample_d != nullptr) 
				{
					hr = ResizeFrameBuffer(len_d);
					int bytesPerPixel = abs(d_Stride) / d_Width;
					//Copy the bitmap buffer, with handling of negative stride. https://docs.microsoft.com/en-us/windows/win32/medfound/image-stride
					hr = MFCopyImage(
						m_PtrFrameBuffer,       // Destination buffer.
						abs(d_Stride),          // Destination stride. We use the absolute value to flip bitmaps with negative stride. 
						d_Stride > 0 ? data_d : data_d + (d_Height - 1) * abs(d_Stride), // First row in source image with positive stride, or the last row with negative stride.
						d_Stride,               // Source stride.
						d_Width * bytesPerPixel,        // Image width in bytes.
						d_Height						// Image height in pixels.
					);	
				}
				if (m_Sample_d) {
					CComPtr<ID3D11Texture2D> pTexture;
					hr = m_TextureManager->CreateTextureFromBuffer(m_PtrFrameBuffer, d_Stride, d_Width, d_Height, &pTexture, 0, D3D11_BIND_SHADER_RESOURCE);
					if (SUCCEEDED(hr)) {
						*ppFrame = pTexture;
						(*ppFrame)->AddRef();
						QueryPerformanceCounter(&m_LastGrabTimeStamp);
					}
				}
			}
		}
	}
	else if (result == WAIT_TIMEOUT) {
		hr = DXGI_ERROR_WAIT_TIMEOUT;
	}
	else {
		DWORD dwErr = GetLastError();
		LOG_ERROR(L"WaitForSingleObject failed: last error = %u", dwErr);
		hr = HRESULT_FROM_WIN32(dwErr);
	}
	return hr;
}

HRESULT DshowCapture::ResizeFrameBuffer(UINT bufferSize) {
	// Old buffer too small
	if (bufferSize > m_BufferSize)
	{
		if (m_PtrFrameBuffer)
		{
			delete[] m_PtrFrameBuffer;
			m_PtrFrameBuffer = nullptr;
		}
		m_PtrFrameBuffer = new (std::nothrow) BYTE[bufferSize];
		if (!m_PtrFrameBuffer)
		{
			m_BufferSize = 0;
			LOG_ERROR(L"Failed to allocate memory for frame");
			return E_OUTOFMEMORY;
		}

		// Update buffer size
		m_BufferSize = bufferSize;
	}
	return S_OK;
}

HRESULT DshowCapture::Initialize(_In_ ID3D11DeviceContext *pDeviceContext, _In_ ID3D11Device *pDevice)
{
	//VideoCamLibInitialize();

	m_Device = pDevice;
	m_DeviceContext = pDeviceContext;

	m_Device->AddRef();
	m_DeviceContext->AddRef();

	m_TextureManager = make_unique<TextureManager>();
	m_TextureManager->Initialize(m_DeviceContext, m_Device);

	if (!m_DeviceManager) {
		RETURN_ON_BAD_HR(MFCreateDXGIDeviceManager(&m_ResetToken, &m_DeviceManager));
	}
	RETURN_ON_BAD_HR(m_DeviceManager->ResetDevice(pDevice, m_ResetToken));
	return S_OK;
}
HRESULT DshowCapture::WriteNextFrameToSharedSurface(_In_ DWORD timeoutMillis, _Inout_ ID3D11Texture2D *pSharedSurf, INT offsetX, INT offsetY, _In_ RECT destinationRect)
{
	CComPtr<ID3D11Texture2D> pProcessedTexture;
	HRESULT hr = AcquireNextFrame(timeoutMillis, &pProcessedTexture);
	RETURN_ON_BAD_HR(hr);

	D3D11_TEXTURE2D_DESC frameDesc;
	pProcessedTexture->GetDesc(&frameDesc);
	RECORDING_SOURCE *recordingSource = dynamic_cast<RECORDING_SOURCE *>(m_RecordingSource);
	if (recordingSource && recordingSource->SourceRect.has_value()
		&& IsValidRect(recordingSource->SourceRect.value())
		&& (RectWidth(recordingSource->SourceRect.value()) != frameDesc.Width || (RectHeight(recordingSource->SourceRect.value()) != frameDesc.Height))) {
		ID3D11Texture2D *pCroppedTexture;
		RETURN_ON_BAD_HR(hr = m_TextureManager->CropTexture(pProcessedTexture, recordingSource->SourceRect.value(), &pCroppedTexture));
		if (hr == S_OK) {
			pProcessedTexture.Release();
			pProcessedTexture.Attach(pCroppedTexture);
		}
	}
	pProcessedTexture->GetDesc(&frameDesc);

	RECT contentRect = destinationRect;
	if (RectWidth(destinationRect) != frameDesc.Width || RectHeight(destinationRect) != frameDesc.Height) {
		ID3D11Texture2D *pResizedTexture;
		RETURN_ON_BAD_HR(hr = m_TextureManager->ResizeTexture(pProcessedTexture, SIZE{ RectWidth(destinationRect),RectHeight(destinationRect) }, m_RecordingSource->Stretch, &pResizedTexture, &contentRect));
		pProcessedTexture.Release();
		pProcessedTexture.Attach(pResizedTexture);
	}

	pProcessedTexture->GetDesc(&frameDesc);

	SIZE contentOffset = GetContentOffset(m_RecordingSource->Anchor, destinationRect, contentRect);
	long left = destinationRect.left + offsetX + contentOffset.cx;
	long top = destinationRect.top + offsetY + contentOffset.cy;
	long right = left + MakeEven(frameDesc.Width);
	long bottom = top + MakeEven(frameDesc.Height);

	m_TextureManager->DrawTexture(pSharedSurf, pProcessedTexture, RECT{ left,top,right,bottom });
	return hr;
}

//From IUnknown 
STDMETHODIMP DshowCapture::QueryInterface(REFIID riid, void **ppvObject)
{
	static const QITAB qit[] = { QITABENT(SourceReaderBase, IMFSourceReaderCallback),{ 0 }, };
	return QISearch(this, qit, riid, ppvObject);
}
//From IUnknown
ULONG DshowCapture::Release()
{
	ULONG count = InterlockedDecrement(&m_ReferenceCount);
	if (count == 0)
		delete this;
	// For thread safety
	return count;
}
//From IUnknown
ULONG DshowCapture::AddRef()
{
	return InterlockedIncrement(&m_ReferenceCount);
}
