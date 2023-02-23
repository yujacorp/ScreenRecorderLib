#pragma once
#include "CommonTypes.h"
#include "Log.h"
#include "Util.h"
#include "MF.util.h"
#include "CMFSinkWriterCallback.h"
#include "cleanup.h"
#include "fifo_map.h"
#include <mfreadwrite.h>

struct FrameWriteModel
{
	//Timestamp of the start of the frame, in 100 nanosecond units.
	INT64 StartPos;
	//Duration of the frame, in 100 nanosecond units.
	INT64 Duration;
	//The audio sample bytes for this frame.
	std::vector<BYTE> Audio;
	//The frame texture.
	CComPtr<ID3D11Texture2D> Frame;
};

class OutputManager
{
public:
	OutputManager();
	~OutputManager();
	HRESULT Initialize(
		_In_ ID3D11DeviceContext *pDeviceContext,
		_In_ ID3D11Device *pDevice,
		_In_ std::shared_ptr<ENCODER_OPTIONS> &pEncoderOptions,
		_In_ std::shared_ptr<AUDIO_OPTIONS> pAudioOptions,
		_In_ std::shared_ptr<SNAPSHOT_OPTIONS> pSnapshotOptions,
		_In_ std::shared_ptr<OUTPUT_OPTIONS> pOutputOptions);

	HRESULT BeginRecording(_In_ std::wstring outputPath, _In_ SIZE videoOutputFrameSizer);
	HRESULT BeginRecording(_In_ IStream *pStream, _In_ SIZE videoOutputFrameSize);
	HRESULT FinalizeRecording();
	HRESULT RenderFrame(_In_ FrameWriteModel &model);
	void WriteTextureToImageAsync(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ std::wstring filePath, _In_opt_ std::function<void(HRESULT)> onCompletion = nullptr);
	inline nlohmann::fifo_map<std::wstring, int> GetFrameDelays() { return m_FrameDelays; }
	inline UINT64 GetRenderedFrameCount() { return m_RenderedFrameCount; }
	void OutputManager::SetScaleWidthAndHeight(UINT32 scaledWidth, UINT32 scaledHeight, bool isScalingEnabled);
	DUPL_RETURN OutputManager::InitOutput(HWND Window, INT SingleOutput, _Out_ UINT *OutCount, _Out_ RECT *DeskBounds);
	DUPL_RETURN OutputManager::MakeRTV();
	DUPL_RETURN OutputManager::UpdateApplicationWindow(_In_ PTR_INFO *PointerInfo, _Inout_ bool *Occluded);
	DUPL_RETURN OutputManager::DrawFrame();
	HRESULT OutputManager::WriteFrameToBuffer(_In_ ID3D11Texture2D *image, _Out_ BYTE **buffer, _Out_ long *width, _Out_ long *height);
	DUPL_RETURN OutputManager::ResizeSwapChain();
	HANDLE OutputManager::GetSharedHandle();
	void OutputManager::CleanRefs();
	void OutputManager::WindowResize();
	void OutputManager::SetDeviceId(std::wstring id);
	int CurrentAudioVolume = 0;
	void OutputManager::SetRenderingParamerters(ID3D11VertexShader* vertexShader, ID3D11PixelShader* pixelShader, ID3D11SamplerState* samplerLinear);
	HRESULT WriteFrameToImage(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ std::wstring filePath);
	DUPL_RETURN OutputManager::ProcessMonoMask(bool IsMono, _Inout_ PTR_INFO *PtrInfo, _Out_ INT *PtrWidth, _Out_ INT *PtrHeight, _Out_ INT *PtrLeft, _Out_ INT *PtrTop, _Outptr_result_bytebuffer_(*PtrHeight **PtrWidth *BPP) BYTE **InitBuffer, _Out_ D3D11_BOX *Box, _In_ ID3D11Texture2D *pBgTexture);
	DUPL_RETURN OutputManager::DrawMouse(_In_ PTR_INFO *PtrInfo, _Inout_ ID3D11Texture2D *pBgTexture);
	void OutputManager::SetMousePtrInfo(_In_ PTR_INFO *PtrInfo);
	HRESULT StartMediaClock();
	HRESULT ResumeMediaClock();
	HRESULT PauseMediaClock();
	HRESULT StopMediaClock();
	HRESULT GetMediaTimeStamp(_Out_ INT64 *pTime);
	bool isMediaClockRunning();
	bool isMediaClockPaused();
private:
	ID3D11DeviceContext *m_DeviceContext = nullptr;
	ID3D11Device *m_Device = nullptr;

	CComPtr<IMFPresentationTimeSource> m_TimeSrc;
	CComPtr<IMFPresentationClock> m_PresentationClock;

	std::shared_ptr<ENCODER_OPTIONS> m_EncoderOptions;
	std::shared_ptr<AUDIO_OPTIONS> m_AudioOptions;
	std::shared_ptr<SNAPSHOT_OPTIONS> m_SnapshotOptions;
	std::shared_ptr<OUTPUT_OPTIONS> m_OutputOptions;

	nlohmann::fifo_map<std::wstring, int> m_FrameDelays;

	CComPtr<IMFSinkWriter> m_SinkWriter;
	CComPtr<IMFSinkWriterCallback> m_CallBack;
	CComPtr<IMFTransform> m_MediaTransform;
	CComPtr<IMFDXGIDeviceManager> m_DeviceManager;
	UINT m_ResetToken;
	IStream *m_OutStream;
	DWORD m_VideoStreamIndex;
	DWORD m_AudioStreamIndex;
	HANDLE m_FinalizeEvent;
	std::wstring m_OutputFolder;
	std::wstring m_OutputFullPath;
	bool m_LastFrameHadAudio;
	UINT64 m_RenderedFrameCount;
	std::chrono::steady_clock::time_point m_PreviousSnapshotTaken;
	CRITICAL_SECTION m_CriticalSection;

	std::shared_ptr<AUDIO_OPTIONS> GetAudioOptions() { return m_AudioOptions; }
	std::shared_ptr<ENCODER_OPTIONS> GetEncoderOptions() { return m_EncoderOptions; }
	std::shared_ptr<SNAPSHOT_OPTIONS> GetSnapshotOptions() { return m_SnapshotOptions; }
	std::shared_ptr<OUTPUT_OPTIONS> GetOutputOptions() { return m_OutputOptions; }

	HRESULT ConfigureOutputMediaTypes(_In_ UINT destWidth, _In_ UINT destHeight, _Outptr_ IMFMediaType **pVideoMediaTypeOut, _Outptr_result_maybenull_ IMFMediaType **pAudioMediaTypeOut);
	HRESULT ConfigureInputMediaTypes(_In_ UINT sourceWidth, _In_ UINT sourceHeight, _In_ MFVideoRotationFormat rotationFormat, _In_ IMFMediaType *pVideoMediaTypeOut, _Outptr_ IMFMediaType **pVideoMediaTypeIn, _Outptr_result_maybenull_ IMFMediaType **pAudioMediaTypeIn);
	HRESULT InitializeVideoSinkWriter(_In_ IMFByteStream *pOutStream, _In_ RECT sourceRect, _In_ SIZE outputFrameSize, _In_ DXGI_MODE_ROTATION rotation, _In_ IMFSinkWriterCallback *pCallback, _Outptr_ IMFSinkWriter **ppWriter, _Out_ DWORD *pVideoStreamIndex, _Out_ DWORD *pAudioStreamIndex);
	HRESULT WriteFrameToVideo(_In_ INT64 frameStartPos, _In_ INT64 frameDuration, _In_ DWORD streamIndex, _In_ ID3D11Texture2D *pAcquiredDesktopImage);
	//HRESULT WriteFrameToImage(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ std::wstring filePath);
	HRESULT WriteFrameToImage(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ IStream *pStream);
	HRESULT WriteAudioSamplesToVideo(_In_ INT64 frameStartPos, _In_ INT64 frameDuration, _In_ DWORD streamIndex, _In_ BYTE *pSrc, _In_ DWORD cbData, _In_ bool shouldWrite = true);
	UINT32 m_ScaledWidth = 0;
	UINT32 m_ScaledHeight = 0;
	HWND m_WindowHandle;
	bool m_NeedsResize;
	IDXGIFactory2 *m_Factory;
	DWORD m_OcclusionCookie;
	IDXGISwapChain1 *m_SwapChain;
	ID3D11RenderTargetView *m_RTV;
	bool m_IsScalingEnabled = false;
	ID3D11SamplerState *m_SamplerLinear;
	ID3D11VertexShader *m_VertexShader;
	ID3D11PixelShader *m_PixelShader;
	ID3D11Texture2D *m_SharedSurf;
	ID3D11InputLayout *m_InputLayout;
	UINT PreviousWindowWidth;
	UINT PreviousWindowHeight;
	std::wstring device_id;
	ID3D11BlendState *m_BlendState;
	PTR_INFO *m_PtrInfo;
	//IDXGIKeyedMutex *m_KeyMutex; 
};

