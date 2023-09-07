#include "OutputManager.h"
#include "screengrab.h"
#include <ppltasks.h> 
#include <concrt.h>
#include <filesystem>
using namespace std;
using namespace concurrency;
using namespace DirectX;

#define USE_NV12_CONVERTER TRUE

OutputManager::OutputManager() :
	m_Device(nullptr),
	m_DeviceContext(nullptr),
	m_PresentationClock(nullptr),
	m_TimeSrc(nullptr),
	m_CallBack(nullptr),
	m_FinalizeEvent(nullptr),
	m_SinkWriter(nullptr),
	m_Sink(nullptr),
	m_OutStream(nullptr),
	m_EncoderOptions(nullptr),
	m_AudioOptions(nullptr),
	m_SnapshotOptions(nullptr),
	m_OutputOptions(nullptr),
	m_VideoStreamIndex(0),
	m_AudioStreamIndex(0),
	m_OutputFolder(L""),
	m_OutputFullPath(L""),
	m_LastFrameHadAudio(false),
	m_RenderedFrameCount(0),
	m_MediaTransform(nullptr),
	m_RTV(nullptr),
	m_SwapChain(nullptr),
	m_SharedSurf(nullptr),
	m_Factory(nullptr),
	m_OcclusionCookie(0),
	m_SamplerLinear(nullptr),
	m_PixelShader(nullptr),
	m_VertexShader(nullptr),
	m_WindowHandle(nullptr),
	m_NeedsResize(false),
	m_InputLayout(nullptr),
	m_BlendState(nullptr),
	m_PtrInfo(nullptr),
	m_DeviceManager(nullptr),
	m_ResetToken(0)
{
	m_FinalizeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	InitializeCriticalSection(&m_CriticalSection);
}

OutputManager::~OutputManager()
{
	CleanRefs();
	CloseHandle(m_FinalizeEvent);
	m_FinalizeEvent = nullptr;
	m_CallBack = nullptr;
	m_MediaTransform = nullptr;
	m_SinkWriter = nullptr;
	m_Sink = nullptr;
	m_DeviceManager = nullptr;
	m_TimeSrc = nullptr;
	m_PresentationClock = nullptr;

	DeleteCriticalSection(&m_CriticalSection);
}

HRESULT OutputManager::Initialize(
	_In_ ID3D11DeviceContext *pDeviceContext,
	_In_ ID3D11Device *pDevice,
	_In_ std::shared_ptr<ENCODER_OPTIONS> &pEncoderOptions,
	_In_ std::shared_ptr<AUDIO_OPTIONS> pAudioOptions,
	_In_ std::shared_ptr<SNAPSHOT_OPTIONS> pSnapshotOptions,
	_In_ std::shared_ptr<OUTPUT_OPTIONS> pOutputOptions)
{
	EnterCriticalSection(&m_CriticalSection);
	LeaveCriticalSectionOnExit leaveOnExit(&m_CriticalSection);

	m_DeviceContext = pDeviceContext;
	m_Device = pDevice;
	m_EncoderOptions = pEncoderOptions;
	m_AudioOptions = pAudioOptions;
	m_SnapshotOptions = pSnapshotOptions;
	m_OutputOptions = pOutputOptions;
	if (!m_DeviceManager) {
		RETURN_ON_BAD_HR(MFCreateDXGIDeviceManager(&m_ResetToken, &m_DeviceManager));
	}
	
	if (!m_TimeSrc) {
		RETURN_ON_BAD_HR(MFCreateSystemTimeSource(&m_TimeSrc));
	}
	if (!m_PresentationClock) {
		RETURN_ON_BAD_HR(MFCreatePresentationClock(&m_PresentationClock));
		RETURN_ON_BAD_HR(m_PresentationClock->SetTimeSource(m_TimeSrc));
	}
	RETURN_ON_BAD_HR(m_DeviceManager->ResetDevice(pDevice, m_ResetToken));
	return S_OK;
}

HRESULT OutputManager::BeginRecording(_In_ std::wstring outputPath, _In_ SIZE videoOutputFrameSize)
{
	HRESULT hr = S_FALSE;
	m_OutputFullPath = outputPath;
	if (outputPath.empty()) {
		LOG_ERROR("Failed to start recording due to output path parameter being empty");
		return E_INVALIDARG;
	}
	std::filesystem::path filePath = outputPath;
	m_OutputFolder = filePath.has_extension() ? filePath.parent_path().wstring() : filePath.wstring();
	ResetEvent(m_FinalizeEvent);

	if (GetOutputOptions()->GetRecorderMode() == RecorderModeInternal::Video) {
		if (m_FinalizeEvent) {
			m_CallBack = new (std::nothrow)CMFSinkWriterCallback(m_FinalizeEvent, nullptr);
		}
		RECT inputMediaFrameRect = RECT{ 0,0,videoOutputFrameSize.cx,videoOutputFrameSize.cy };
		CComPtr<IMFByteStream> mfByteStream = nullptr;
		if (GetOutputOptions()->GetIsPreviewOnly())
		{
			RETURN_ON_BAD_HR(MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, &mfByteStream));
		}
		else
		{
			RETURN_ON_BAD_HR(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, outputPath.c_str(), &mfByteStream));
		}
		
		RETURN_ON_BAD_HR(hr = InitializeVideoSinkWriter(mfByteStream, inputMediaFrameRect, videoOutputFrameSize, DXGI_MODE_ROTATION_UNSPECIFIED, m_CallBack, &m_SinkWriter, &m_VideoStreamIndex, &m_AudioStreamIndex));
	}
	StartMediaClock();
	LOG_DEBUG("Sink Writer initialized");
	return hr;
}

HRESULT OutputManager::BeginRecording(_In_ IStream *pStream, _In_ SIZE videoOutputFrameSize)
{
	HRESULT hr = S_FALSE;
	if (pStream == nullptr) {
		LOG_ERROR("Failed to start recording due to output stream parameter being NULL");
		return E_INVALIDARG;
	}
	m_OutStream = pStream;
	ResetEvent(m_FinalizeEvent);
	if (GetOutputOptions()->GetRecorderMode() == RecorderModeInternal::Video) {
		CComPtr<IMFByteStream> mfByteStream = nullptr;
		RETURN_ON_BAD_HR(hr = MFCreateMFByteStreamOnStream(pStream, &mfByteStream));

		if (m_FinalizeEvent) {
			m_CallBack = new (std::nothrow)CMFSinkWriterCallback(m_FinalizeEvent, nullptr);
		}
		RECT inputMediaFrameRect = RECT{ 0,0,videoOutputFrameSize.cx,videoOutputFrameSize.cy };
		RETURN_ON_BAD_HR(hr = InitializeVideoSinkWriter(mfByteStream, inputMediaFrameRect, videoOutputFrameSize, DXGI_MODE_ROTATION_UNSPECIFIED, m_CallBack, &m_SinkWriter, &m_VideoStreamIndex, &m_AudioStreamIndex));
	}
	StartMediaClock();
	LOG_DEBUG("Sink Writer initialized");
	return hr;
}

HRESULT OutputManager::FinalizeRecording()
{
	LOG_INFO("Cleaning up resources");
	LOG_INFO("Finalizing recording");
	CleanRefs();
	HRESULT finalizeResult = S_OK;
	if (m_SinkWriter) {

		finalizeResult = m_SinkWriter->Finalize();
		if (SUCCEEDED(finalizeResult) && m_FinalizeEvent) {
			WaitForSingleObject(m_FinalizeEvent, INFINITE);
		}
		if (FAILED(finalizeResult)) {
			LOG_ERROR("Failed to finalize sink writer");
		}
		//Dispose of MPEG4MediaSink 
		if (m_Sink) 
		{
			m_SinkWriter = nullptr;
			finalizeResult = m_Sink->Shutdown();
			m_Sink = nullptr;
			if (FAILED(finalizeResult)) {
				LOG_ERROR("Failed to shut down IMFMediaSink");
			}
			else {
				LOG_DEBUG("Shut down IMFMediaSink");
			}
		}
		m_SinkWriter = nullptr;
		
		if (!m_OutputFullPath.empty()) {
			bool isFileAvailable = false;
			for (int i = 0; i < 10; i++) {
				isFileAvailable = IsFileAvailableForReading(m_OutputFullPath);
				if (isFileAvailable) {
					LOG_TRACE(L"Output file is ready");
					break;
				}
				else {
					Sleep(100);
					LOG_TRACE(L"Output file is still locked for reading, waiting..");
				}
			}
			if (!isFileAvailable) {
				LOG_WARN("Output file is still locked after maximum retries");
			}
		}
	}
	StopMediaClock();
	return finalizeResult;
}

HRESULT OutputManager::RenderFrame(_In_ FrameWriteModel &model) {
	HRESULT hr(S_OK);
	EnterCriticalSection(&m_CriticalSection);
	LeaveCriticalSectionOnExit leaveOnExit(&m_CriticalSection);
	MeasureExecutionTime measure(L"RenderFrame");
	auto recorderMode = GetOutputOptions()->GetRecorderMode();
	bool isPreviewOnly = GetOutputOptions()->GetIsPreviewOnly();
	if (m_PtrInfo && m_PtrInfo->Visible && m_PtrInfo->PtrShapeBuffer != nullptr) {
		DUPL_RETURN ret = DrawMouse(m_PtrInfo, model.Frame);
		if (ret != DUPL_RETURN_SUCCESS) {
			LOG_ERROR(L"Error drawing mouse pointer");
			//We just log the error and continue if the mouse pointer failed to draw. If there is an error with DXGI, it will be handled on the next call to AcquireNextFrame.
		}
	}
	
	if (recorderMode == RecorderModeInternal::Video && !isPreviewOnly) {

		D3D11_TEXTURE2D_DESC sourceFrameDesc;
		model.Frame->GetDesc(&sourceFrameDesc);

		D3D11_BOX sourceRegion;
		RtlZeroMemory(&sourceRegion, sizeof(sourceRegion));
		sourceRegion.left = 0; // for some reason, if scaling is enabled, we don't have to set any offset for capture area.
		sourceRegion.right = m_IsScalingEnabled ? m_ScaledWidth : sourceFrameDesc.Width;
		sourceRegion.top = 0;  // for some reason, if scaling is enabled, we don't have to set any offset for capture area.
		sourceRegion.bottom = m_IsScalingEnabled ? m_ScaledHeight : sourceFrameDesc.Height;
		sourceRegion.front = 0;
		sourceRegion.back = 1;

		if (m_SharedSurf != nullptr) {
			m_DeviceContext->CopySubresourceRegion(m_SharedSurf, 0, 0, 0, 0, model.Frame, 0, &sourceRegion);
		}

		bool Occluded = false;
		if (m_SharedSurf != nullptr) {
			DUPL_RETURN Ret = UpdateApplicationWindow(NULL, &Occluded);
		}

		/*if (m_SharedSurf != nullptr) {
			m_DeviceContext->CopySubresourceRegion(model.Frame, 0, 0, 0, 0, m_SharedSurf, 0, &sourceRegion);
		}*/

		/*WriteFrameToImage(model.Frame, L"D:\\test\\model-"+ device_id +L"-.png");
		WriteFrameToImage(m_SharedSurf, L"D:\\test\\shared-" + device_id + L"-.png");*/
		/*WriteFrameToImage(model.Frame, L"D:\\test\\model.png");
		WriteFrameToImage(m_SharedSurf, L"D:\\test\\shared.png");*/
		model.Duration = model.Duration;

		hr = WriteFrameToVideo(model.StartPos, model.Duration, m_VideoStreamIndex, model.Frame);
		bool wroteAudioSample = false;
		if (FAILED(hr)) {
			_com_error err(hr);
			LOG_ERROR(L"Writing of video frame with start pos %lld ms failed: %s", (HundredNanosToMillis(model.StartPos)), err.ErrorMessage());
			return hr;//Stop recording if we fail
		}
		bool paddedAudio = false;

		/* If the audio pCaptureInstance returns no data, i.e. the source is silent, we need to pad the PCM stream with zeros to give the media sink silence as input.
		 * If we don't, the sink writer will begin throttling video frames because it expects audio samples to be delivered, and think they are delayed.
		 * We ignore every instance where the last frame had audio, due to sometimes very short frame durations due to mouse cursor changes have zero audio length,
		 * and inserting silence between two frames that has audio leads to glitching. */
		if (GetAudioOptions()->IsAudioEnabled() && model.Audio.size() == 0 && model.Duration > 0) {
			if (!m_LastFrameHadAudio) {
				int frameCount = int(ceil(GetAudioOptions()->GetAudioSamplesPerSecond() * HundredNanosToMillis(model.Duration) / 1000));
				int byteCount = frameCount * (GetAudioOptions()->GetAudioBitsPerSample() / 8) * GetAudioOptions()->GetAudioChannels();
				model.Audio.insert(model.Audio.end(), byteCount, 0);
				paddedAudio = true;
			}
			m_LastFrameHadAudio = false;
		}
		else {
			m_LastFrameHadAudio = true;
		}

		if (model.Audio.size() > 0) {
			hr = WriteAudioSamplesToVideo(model.StartPos, model.Duration, m_AudioStreamIndex, &(model.Audio)[0], (DWORD)model.Audio.size());
			if (FAILED(hr)) {
				_com_error err(hr);
				LOG_ERROR(L"Writing of audio sample with start pos %lld ms failed: %s", (HundredNanosToMillis(model.StartPos)), err.ErrorMessage());
				return hr;//Stop recording if we fail
			}
			else {
				wroteAudioSample = true;
			}
		}
		auto frameInfoStr = wroteAudioSample ? (paddedAudio ? L"video sample and audio padding" : L"video and audio sample") : L"video sample";
		LOG_TRACE(L"Wrote %s with duration %.2f ms", frameInfoStr, HundredNanosToMillisDouble(model.Duration));
	}
	else if (recorderMode == RecorderModeInternal::Slideshow) {
		wstring	path = m_OutputFolder + L"\\" + to_wstring(m_RenderedFrameCount) + GetSnapshotOptions()->GetImageExtension();
		hr = WriteFrameToImage(model.Frame, path);
		INT64 startposMs = HundredNanosToMillis(model.StartPos);
		INT64 durationMs = HundredNanosToMillis(model.Duration);
		if (FAILED(hr)) {
			_com_error err(hr);
			LOG_ERROR(L"Writing of slideshow frame with start pos %lld ms failed: %s", startposMs, err.ErrorMessage());
			return hr; //Stop recording if we fail
		}
		else {

			m_FrameDelays.insert(std::pair<wstring, int>(path, m_RenderedFrameCount == 0 ? 0 : (int)durationMs));
			LOG_TRACE(L"Wrote video slideshow frame with start pos %lld ms and with duration %lld ms", startposMs, durationMs);
		}
	}
	else if (recorderMode == RecorderModeInternal::Screenshot) {
		if (m_OutStream) {
			hr = WriteFrameToImage(model.Frame, m_OutStream);
			LOG_TRACE(L"Wrote snapshot to stream");
		}
		else {
			hr = WriteFrameToImage(model.Frame, m_OutputFullPath);
			LOG_TRACE(L"Wrote snapshot to %s", m_OutputFullPath.c_str());
		}
	}
	else if (isPreviewOnly) {
		D3D11_TEXTURE2D_DESC sourceFrameDesc;
		model.Frame->GetDesc(&sourceFrameDesc);

		D3D11_BOX sourceRegion;
		RtlZeroMemory(&sourceRegion, sizeof(sourceRegion));
		sourceRegion.left = 0; // for some reason, if scaling is enabled, we don't have to set any offset for capture area.
		sourceRegion.right = m_IsScalingEnabled ? m_ScaledWidth : sourceFrameDesc.Width;
		sourceRegion.top = 0;  // for some reason, if scaling is enabled, we don't have to set any offset for capture area.
		sourceRegion.bottom = m_IsScalingEnabled ? m_ScaledHeight : sourceFrameDesc.Height;
		sourceRegion.front = 0;
		sourceRegion.back = 1;

		if (m_SharedSurf != nullptr) {
			m_DeviceContext->CopySubresourceRegion(m_SharedSurf, 0, 0, 0, 0, model.Frame, 0, &sourceRegion);
		}

		/*WriteFrameToImage(model.Frame, L"D:\\test\\model-"+ device_id +L"-.png");
		WriteFrameToImage(m_SharedSurf, L"D:\\test\\shared-" + device_id + L"-.png");*/

		bool Occluded = false;
		if (m_SharedSurf != nullptr) {
			DUPL_RETURN Ret = UpdateApplicationWindow(NULL, &Occluded);
		}
		hr = S_OK;

		bool paddedAudio = false;
		bool wroteAudioSample = false;
		/* If the audio pCaptureInstance returns no data, i.e. the source is silent, we need to pad the PCM stream with zeros to give the media sink silence as input.
		 * If we don't, the sink writer will begin throttling video frames because it expects audio samples to be delivered, and think they are delayed.
		 * We ignore every instance where the last frame had audio, due to sometimes very short frame durations due to mouse cursor changes have zero audio length,
		 * and inserting silence between two frames that has audio leads to glitching. */
		if (GetAudioOptions()->IsAudioEnabled() && model.Audio.size() == 0 && model.Duration > 0) {
			if (!m_LastFrameHadAudio) {
				int frameCount = int(ceil(GetAudioOptions()->GetAudioSamplesPerSecond() * HundredNanosToMillis(model.Duration) / 1000));
				int byteCount = frameCount * (GetAudioOptions()->GetAudioBitsPerSample() / 8) * GetAudioOptions()->GetAudioChannels();
				model.Audio.insert(model.Audio.end(), byteCount, 0);
				paddedAudio = true;
			}
			m_LastFrameHadAudio = false;
		}
		else {
			m_LastFrameHadAudio = true;
		}

		if (model.Audio.size() > 0) {
			hr = WriteAudioSamplesToVideo(model.StartPos, model.Duration, m_AudioStreamIndex, &(model.Audio)[0], (DWORD)model.Audio.size(), false);
			if (FAILED(hr)) {
				_com_error err(hr);
				LOG_ERROR(L"Writing of audio sample with start pos %lld ms failed: %s", (HundredNanosToMillis(model.StartPos)), err.ErrorMessage());
				return hr;//Stop recording if we fail
			}
			else {
				wroteAudioSample = true;
			}
		}
	}
	model.Frame.Release();
	m_RenderedFrameCount++;
	return hr;
}

HRESULT OutputManager::WriteFrameToImage(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ std::wstring filePath)
{
	return SaveWICTextureToFile(m_DeviceContext, pAcquiredDesktopImage, GetSnapshotOptions()->GetSnapshotEncoderFormat(), filePath.c_str());
}
HRESULT OutputManager::WriteFrameToImage(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ IStream *pStream)
{
	return SaveWICTextureToStream(m_DeviceContext, pAcquiredDesktopImage, GetSnapshotOptions()->GetSnapshotEncoderFormat(), pStream);
}
void OutputManager::WriteTextureToImageAsync(_In_ ID3D11Texture2D *pAcquiredDesktopImage, _In_ std::wstring filePath, _In_opt_ std::function<void(HRESULT)> onCompletion)
{
	pAcquiredDesktopImage->AddRef();
	Concurrency::create_task([this, pAcquiredDesktopImage, filePath, onCompletion]() {
		return WriteFrameToImage(pAcquiredDesktopImage, filePath);
	   }).then([this, filePath, pAcquiredDesktopImage, onCompletion](concurrency::task<HRESULT> t)
		   {
			   HRESULT hr;
	   try {
		   hr = t.get();
		   // if .get() didn't throw and the HRESULT succeeded, there are no errors.
	   }
	   catch (const exception &e) {
		   // handle error
		   LOG_ERROR(L"Exception saving snapshot: %s", s2ws(e.what()).c_str());
		   hr = E_FAIL;
	   }
	   pAcquiredDesktopImage->Release();
	   if (onCompletion) {
		   std::invoke(onCompletion, hr);
	   }
	   return hr;
		   });
}

HRESULT OutputManager::StartMediaClock()
{
	return m_PresentationClock->Start(0);
}
HRESULT OutputManager::ResumeMediaClock()
{
	return m_PresentationClock->Start(PRESENTATION_CURRENT_POSITION);
}
HRESULT OutputManager::PauseMediaClock()
{
	return m_PresentationClock->Pause();
}
HRESULT OutputManager::StopMediaClock()
{
	return m_PresentationClock->Stop();
}

bool OutputManager::isMediaClockRunning()
{
	MFCLOCK_STATE state;
	m_PresentationClock->GetState(0, &state);
	return state == MFCLOCK_STATE_RUNNING;
}

bool OutputManager::isMediaClockPaused()
{
	MFCLOCK_STATE state;
	m_PresentationClock->GetState(0, &state);
	return state == MFCLOCK_STATE_PAUSED;
}

HRESULT OutputManager::GetMediaTimeStamp(_Out_ INT64 *pTime)
{
	return m_PresentationClock->GetTime(pTime);
}

HRESULT OutputManager::ConfigureOutputMediaTypes(
	_In_ UINT destWidth,
	_In_ UINT destHeight,
	_Outptr_ IMFMediaType **pVideoMediaTypeOut,
	_Outptr_result_maybenull_ IMFMediaType **pAudioMediaTypeOut)
{
	*pVideoMediaTypeOut = nullptr;
	*pAudioMediaTypeOut = nullptr;
	CComPtr<IMFMediaType> pVideoMediaType = nullptr;
	CComPtr<IMFMediaType> pAudioMediaType = nullptr;
	// Set the output video type.
	RETURN_ON_BAD_HR(MFCreateMediaType(&pVideoMediaType));
	RETURN_ON_BAD_HR(pVideoMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	RETURN_ON_BAD_HR(pVideoMediaType->SetGUID(MF_MT_SUBTYPE, GetEncoderOptions()->GetVideoEncoderFormat()));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_AVG_BITRATE, GetEncoderOptions()->GetVideoBitrate()));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, GetEncoderOptions()->GetEncoderProfile()));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
	RETURN_ON_BAD_HR(MFSetAttributeSize(pVideoMediaType, MF_MT_FRAME_SIZE, destWidth, destHeight));
	RETURN_ON_BAD_HR(MFSetAttributeRatio(pVideoMediaType, MF_MT_FRAME_RATE, GetEncoderOptions()->GetVideoFps(), 1));
	RETURN_ON_BAD_HR(MFSetAttributeRatio(pVideoMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	if (GetAudioOptions()->IsAudioEnabled()) {
		// Set the output audio type.
		RETURN_ON_BAD_HR(MFCreateMediaType(&pAudioMediaType));
		RETURN_ON_BAD_HR(pAudioMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
		RETURN_ON_BAD_HR(pAudioMediaType->SetGUID(MF_MT_SUBTYPE, GetAudioOptions()->GetAudioEncoderFormat()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, GetAudioOptions()->GetAudioChannels()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, GetAudioOptions()->GetAudioBitsPerSample()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, GetAudioOptions()->GetAudioSamplesPerSecond()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, GetAudioOptions()->GetAudioBitrate()));

		*pAudioMediaTypeOut = pAudioMediaType;
		(*pAudioMediaTypeOut)->AddRef();
	}

	*pVideoMediaTypeOut = pVideoMediaType;
	(*pVideoMediaTypeOut)->AddRef();
	return S_OK;
}

HRESULT OutputManager::ConfigureInputMediaTypes(
	_In_ UINT sourceWidth,
	_In_ UINT sourceHeight,
	_In_ MFVideoRotationFormat rotationFormat,
	_In_ IMFMediaType *pVideoMediaTypeOut,
	_Outptr_ IMFMediaType **pVideoMediaTypeIn,
	_Outptr_result_maybenull_ IMFMediaType **pAudioMediaTypeIn)
{
	*pVideoMediaTypeIn = nullptr;
	*pAudioMediaTypeIn = nullptr;
	CComPtr<IMFMediaType> pVideoMediaType = nullptr;
	CComPtr<IMFMediaType> pAudioMediaType = nullptr;

	RETURN_ON_BAD_HR(MFCreateMediaType(&pVideoMediaType));
	RETURN_ON_BAD_HR(pVideoMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	RETURN_ON_BAD_HR(pVideoMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
	// Uncompressed means all samples are independent.
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_VIDEO_ROTATION, rotationFormat));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
	RETURN_ON_BAD_HR(pVideoMediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
	RETURN_ON_BAD_HR(MFSetAttributeSize(pVideoMediaType, MF_MT_FRAME_SIZE, sourceWidth, sourceHeight));
	if (!GetEncoderOptions()->GetIsFixedFramerate() && !GetEncoderOptions()->GetIsFragmentedMp4Enabled()) {
		RETURN_ON_BAD_HR(MFSetAttributeRatio(pVideoMediaType, MF_MT_FRAME_RATE, GetEncoderOptions()->GetVideoFps(), 1));
	}
	RETURN_ON_BAD_HR(MFSetAttributeRatio(pVideoMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	if (GetAudioOptions()->IsAudioEnabled()) {
		// Set the input audio type.
		RETURN_ON_BAD_HR(MFCreateMediaType(&pAudioMediaType));
		RETURN_ON_BAD_HR(pAudioMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
		RETURN_ON_BAD_HR(pAudioMediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, GetAudioOptions()->GetAudioBitsPerSample()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, GetAudioOptions()->GetAudioSamplesPerSecond()));
		RETURN_ON_BAD_HR(pAudioMediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, GetAudioOptions()->GetAudioChannels()));

		*pAudioMediaTypeIn = pAudioMediaType;
		(*pAudioMediaTypeIn)->AddRef();
	}

	*pVideoMediaTypeIn = pVideoMediaType;
	(*pVideoMediaTypeIn)->AddRef();
	return S_OK;
}

HRESULT OutputManager::InitializeVideoSinkWriter(
	_In_ IMFByteStream *pOutStream,
	_In_ RECT sourceRect,
	_In_ SIZE outputFrameSize,
	_In_ DXGI_MODE_ROTATION rotation,
	_In_ IMFSinkWriterCallback *pCallback,
	_Outptr_ IMFSinkWriter **ppWriter,
	_Out_ DWORD *pVideoStreamIndex,
	_Out_ DWORD *pAudioStreamIndex)
{
	*ppWriter = nullptr;
	*pVideoStreamIndex = 0;
	*pAudioStreamIndex = 0;

	CComPtr<IMFSinkWriter>        pSinkWriter = nullptr;
	CComPtr<IMFMediaType>         pVideoMediaTypeOut = nullptr;
	CComPtr<IMFMediaType>         pAudioMediaTypeOut = nullptr;
	CComPtr<IMFMediaType>         pVideoMediaTypeIn = nullptr;
	CComPtr<IMFMediaType>		  pVideoMediaTypeIntermediate = nullptr;
	CComPtr<IMFMediaType>		  pVideoMediaTypeTransform = nullptr;
	CComPtr<IMFMediaType>         pAudioMediaTypeIn = nullptr;
	CComPtr<IMFAttributes>        pAttributes = nullptr;

	MFVideoRotationFormat rotationFormat = MFVideoRotationFormat_0;
	if (rotation == DXGI_MODE_ROTATION_ROTATE90) {
		rotationFormat = MFVideoRotationFormat_90;
	}
	else if (rotation == DXGI_MODE_ROTATION_ROTATE180) {
		rotationFormat = MFVideoRotationFormat_180;
	}
	else if (rotation == DXGI_MODE_ROTATION_ROTATE270) {
		rotationFormat = MFVideoRotationFormat_270;
	}

	DWORD videoStreamIndex = 0;
	DWORD audioStreamIndex = 1;

	UINT sourceWidth = RectWidth(sourceRect);
	UINT sourceHeight = RectHeight(sourceRect);

	UINT destWidth = max(0, outputFrameSize.cx);
	UINT destHeight = max(0, outputFrameSize.cy);

	RETURN_ON_BAD_HR(ConfigureOutputMediaTypes(destWidth, destHeight, &pVideoMediaTypeOut, &pAudioMediaTypeOut));
	RETURN_ON_BAD_HR(ConfigureInputMediaTypes(sourceWidth, sourceHeight, rotationFormat, pVideoMediaTypeOut, &pVideoMediaTypeIn, &pAudioMediaTypeIn));

	//The source samples have the format ARGB32, but the video encoders need the input to be a YUV format, so we convert ARGB32->NV12->H264/HEVC
	CopyMediaType(pVideoMediaTypeIn, &pVideoMediaTypeIntermediate);
	RETURN_ON_BAD_HR(pVideoMediaTypeIntermediate->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
	CopyMediaType(pVideoMediaTypeIntermediate, &pVideoMediaTypeTransform);
	pVideoMediaTypeTransform->DeleteItem(MF_MT_FRAME_RATE);

	RETURN_ON_BAD_HR(CreateIMFTransform(videoStreamIndex, pVideoMediaTypeIn, pVideoMediaTypeTransform, &m_MediaTransform));

	CComPtr<IMFAttributes> pTransformAttributes;
	if (SUCCEEDED(m_MediaTransform->GetAttributes(&pTransformAttributes))) {
		UINT32 d3d11Aware = 0;
		pTransformAttributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
		if (d3d11Aware > 0) {
			HRESULT hr = m_MediaTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_DeviceManager.p));
			LOG_ON_BAD_HR(hr);
		}
	}

	//Creates a streaming writer
	CComPtr<IMFMediaSink> pMp4StreamSink = nullptr;
	if (GetEncoderOptions()->GetIsFragmentedMp4Enabled()) {
		RETURN_ON_BAD_HR(MFCreateFMPEG4MediaSink(pOutStream, pVideoMediaTypeOut, pAudioMediaTypeOut, &pMp4StreamSink));
	}
	else {
		RETURN_ON_BAD_HR(MFCreateMPEG4MediaSink(pOutStream, pVideoMediaTypeOut, pAudioMediaTypeOut, &pMp4StreamSink));
	}
	pAudioMediaTypeOut.Release();

	RETURN_ON_BAD_HR(MFCreateAttributes(&pAttributes, 7));
	RETURN_ON_BAD_HR(pAttributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, GetEncoderOptions()->GetIsFragmentedMp4Enabled() ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4));
	RETURN_ON_BAD_HR(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, GetEncoderOptions()->GetIsHardwareEncodingEnabled()));
	RETURN_ON_BAD_HR(pAttributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, GetEncoderOptions()->GetIsFastStartEnabled()));
	RETURN_ON_BAD_HR(pAttributes->SetUINT32(MF_LOW_LATENCY, GetEncoderOptions()->GetIsLowLatencyModeEnabled()));
	RETURN_ON_BAD_HR(pAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, GetEncoderOptions()->GetIsThrottlingDisabled()));
	// Add device manager to attributes. This enables hardware encoding.
	RETURN_ON_BAD_HR(pAttributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, m_DeviceManager));
	RETURN_ON_BAD_HR(pAttributes->SetUnknown(MF_SINK_WRITER_ASYNC_CALLBACK, pCallback));

	RETURN_ON_BAD_HR(MFCreateSinkWriterFromMediaSink(pMp4StreamSink, pAttributes, &pSinkWriter));
	m_Sink = pMp4StreamSink;

	LOG_TRACE("Input video format:")
		LogMediaType(pVideoMediaTypeIn);
	LOG_TRACE("Converted video format:")
		LogMediaType(pVideoMediaTypeIntermediate);
	LOG_TRACE("Output video format:")
		LogMediaType(pVideoMediaTypeOut);

	RETURN_ON_BAD_HR(pSinkWriter->SetInputMediaType(videoStreamIndex, USE_NV12_CONVERTER ? pVideoMediaTypeIntermediate : pVideoMediaTypeIn, nullptr));
	if (pAudioMediaTypeIn) {
		RETURN_ON_BAD_HR(pSinkWriter->SetInputMediaType(audioStreamIndex, pAudioMediaTypeIn, nullptr));
	}

	auto SetAttributeU32([](_Inout_ CComPtr<ICodecAPI> &codec, _In_ const GUID &guid, _In_ UINT32 value)
	{
		VARIANT val;
	val.vt = VT_UI4;
	val.uintVal = value;
	return codec->SetValue(&guid, &val);
	});

	CComPtr<ICodecAPI> encoder = nullptr;
	pSinkWriter->GetServiceForStream(videoStreamIndex, GUID_NULL, IID_PPV_ARGS(&encoder));
	if (encoder) {
		RETURN_ON_BAD_HR(SetAttributeU32(encoder, CODECAPI_AVEncCommonRateControlMode, GetEncoderOptions()->GetVideoBitrateMode()));
		switch (GetEncoderOptions()->GetVideoBitrateMode()) {
			case eAVEncCommonRateControlMode_Quality:
				RETURN_ON_BAD_HR(SetAttributeU32(encoder, CODECAPI_AVEncCommonQuality, GetEncoderOptions()->GetVideoQuality()));
				break;
			default:
				break;
		}
	}

	// Tell the sink writer to start accepting data.
	RETURN_ON_BAD_HR(pSinkWriter->BeginWriting());

	// Return the pointer to the caller.
	*ppWriter = pSinkWriter;
	(*ppWriter)->AddRef();
	*pVideoStreamIndex = videoStreamIndex;
	*pAudioStreamIndex = audioStreamIndex;
	return S_OK;
}

HRESULT OutputManager::WriteFrameToVideo(_In_ INT64 frameStartPos, _In_ INT64 frameDuration, _In_ DWORD streamIndex, _In_ ID3D11Texture2D *pAcquiredDesktopImage)
{
	IMFMediaBuffer *pMediaBuffer;
	HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pAcquiredDesktopImage, 0, FALSE, &pMediaBuffer);
	IMF2DBuffer *p2DBuffer;
	if (SUCCEEDED(hr))
	{
		hr = pMediaBuffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void **>(&p2DBuffer));
	}
	DWORD length;
	if (SUCCEEDED(hr))
	{
		hr = p2DBuffer->GetContiguousLength(&length);
	}
	if (SUCCEEDED(hr))
	{
		hr = pMediaBuffer->SetCurrentLength(length);
	}
	IMFSample *pSample;
	if (SUCCEEDED(hr))
	{
		hr = MFCreateSample(&pSample);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSample->AddBuffer(pMediaBuffer);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSample->SetSampleTime(frameStartPos);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSample->SetSampleDuration(frameDuration);
	}
#if USE_NV12_CONVERTER 
	//Run media transform to convert sample to MFVideoFormat_NV12

	MFT_OUTPUT_STREAM_INFO info{};

	hr = m_MediaTransform->GetOutputStreamInfo(streamIndex, &info);
	bool transformProvidesSamples = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);

	IMFSample *transformSample = nullptr;

	IMFMediaBuffer *transformBuffer = nullptr;
	MFT_OUTPUT_DATA_BUFFER outputDataBuffer;
	RtlZeroMemory(&outputDataBuffer, sizeof(outputDataBuffer));
	outputDataBuffer.dwStreamID = streamIndex;
	{
		if (SUCCEEDED(hr) && !transformProvidesSamples)
		{
			hr = MFCreateMemoryBuffer(info.cbSize, &transformBuffer);

			if (SUCCEEDED(hr))
			{
				hr = MFCreateSample(&transformSample);
			}
			if (SUCCEEDED(hr)) {
				hr = transformSample->AddBuffer(transformBuffer);
			}
			outputDataBuffer.pSample = transformSample;
			SafeRelease(&transformBuffer);
		}
		if (SUCCEEDED(hr))
		{
			hr = m_MediaTransform->ProcessInput(streamIndex, pSample, 0);
		}
		if (SUCCEEDED(hr))
		{
			DWORD dwDSPStatus = 0;
			hr = m_MediaTransform->ProcessOutput(0, 1, &outputDataBuffer, &dwDSPStatus);
		}
		if (SUCCEEDED(hr)) {
			transformSample = outputDataBuffer.pSample;
		}
	}
	if (SUCCEEDED(hr))
	{
		hr = transformSample->SetSampleTime(frameStartPos);
	}
	if (SUCCEEDED(hr))
	{
		hr = transformSample->SetSampleDuration(frameDuration);
	}
	if (SUCCEEDED(hr))
	{
		hr = m_SinkWriter->WriteSample(streamIndex, transformSample);
	}
	SafeRelease(&transformSample);

#else
	if (SUCCEEDED(hr))
	{
		hr = m_SinkWriter->WriteSample(streamIndex, pSample);
	}
#endif
	SafeRelease(&pSample);
	SafeRelease(&p2DBuffer);
	SafeRelease(&pMediaBuffer);
	return hr;
}

HRESULT OutputManager::WriteAudioSamplesToVideo(_In_ INT64 frameStartPos, _In_ INT64 frameDuration, _In_ DWORD streamIndex, _In_ BYTE *pSrc, _In_ DWORD cbData, _In_ bool shouldWrite)
{
	IMFMediaBuffer *pBuffer = nullptr;
	BYTE *pData = nullptr;
	// Create the media buffer.
	HRESULT hr = MFCreateMemoryBuffer(
		cbData,   // Amount of memory to allocate, in bytes.
		&pBuffer
	);
	//once in awhile, things get behind and we get an out of memory error when trying to create the buffer
	//so, just check, wait and try again if necessary
	int counter = 0;
	while (!SUCCEEDED(hr) && counter++ < 100) {
		Sleep(10);
		hr = MFCreateMemoryBuffer(cbData, &pBuffer);
	}
	// Lock the buffer to get a pointer to the memory.
	if (SUCCEEDED(hr))
	{
		hr = pBuffer->Lock(&pData, nullptr, nullptr);
	}

	if (SUCCEEDED(hr))
	{
		memcpy_s(pData, cbData, pSrc, cbData);
	}

	// Update the current length.
	if (SUCCEEDED(hr))
	{
		hr = pBuffer->SetCurrentLength(cbData);
	}

	// Unlock the buffer.
	if (pData)
	{
		hr = pBuffer->Unlock();
	}

	IMFSample *pSample;
	if (SUCCEEDED(hr))
	{
		hr = MFCreateSample(&pSample);
	}
	if (SUCCEEDED(hr))
	{
		hr = pSample->AddBuffer(pBuffer);
	}
	if (SUCCEEDED(hr))
	{
		INT64 start = frameStartPos;
		hr = pSample->SetSampleTime(start);
	}
	if (SUCCEEDED(hr))
	{
		INT64 duration = frameDuration;
		hr = pSample->SetSampleDuration(duration);
	}
	if (SUCCEEDED(hr) && shouldWrite)
	{
		// Send the sample to the Sink Writer.
		hr = m_SinkWriter->WriteSample(streamIndex, pSample);
	}
	SafeRelease(&pBuffer);

	DWORD pcbCurrentLength;
	DWORD pcbMaxLength;
	BYTE *ppbBuffer;
	hr = pSample->ConvertToContiguousBuffer(&pBuffer);
	hr = pBuffer->Lock(&ppbBuffer, &pcbMaxLength, &pcbCurrentLength);

	int dataPerVolumeCalculation = GetAudioOptions()->GetAudioSamplesPerSecond() / 400;
	int audioDataPoints = 0;
	int audioMagnitude = 0;
	short *b = (short *)ppbBuffer;

	for (int i = 0; i < pcbCurrentLength / 2; i++)
	{
		if (b[i] == SHRT_MIN)
			audioMagnitude += SHRT_MAX;
		else
			audioMagnitude += std::abs(b[i]);

		audioDataPoints++;

		if (audioDataPoints == dataPerVolumeCalculation)
		{
			audioDataPoints = 0;
			int currentVolume = ((audioMagnitude) / dataPerVolumeCalculation);
			if (currentVolume > CurrentAudioVolume)
			{
				CurrentAudioVolume = (int)(currentVolume);
			}
			else
			{
				CurrentAudioVolume = (int)(currentVolume * 0.05 + CurrentAudioVolume * 0.95);
			}
			audioMagnitude = 0;
		}
	}

	pBuffer->Unlock();

	SafeRelease(&pSample);
	SafeRelease(&pBuffer);
	return hr;
}

void OutputManager::SetScaleWidthAndHeight(UINT32 scaledWidth, UINT32 scaledHeight, bool isScalingEnabled)
{
	m_ScaledWidth = scaledWidth;
	m_ScaledHeight = scaledHeight;
	m_IsScalingEnabled = isScalingEnabled;
}

//
// Indicates that window has been resized.
//
void OutputManager::WindowResize()
{
	m_NeedsResize = true;
}

//
// Initialize all state
//
DUPL_RETURN OutputManager::InitOutput(HWND Window, INT SingleOutput, _Out_ UINT *OutCount, _Out_ RECT *DeskBounds)
{
	HRESULT hr;

	// Store window handle
	m_WindowHandle = Window;

	// Get DXGI factory
	IDXGIDevice *DxgiDevice = nullptr;
	hr = m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&DxgiDevice));
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed to QI for DXGI Device", L"Error", hr, nullptr);
	}

	IDXGIAdapter *DxgiAdapter = nullptr;
	hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(&DxgiAdapter));
	DxgiDevice->Release();
	DxgiDevice = nullptr;
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to get parent DXGI Adapter", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	hr = DxgiAdapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&m_Factory));
	DxgiAdapter->Release();
	DxgiAdapter = nullptr;
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to get parent DXGI Factory", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	if (m_OcclusionCookie)
	{
		// Register for occlusion status windows message
		hr = m_Factory->RegisterOcclusionStatusWindow(Window, OCCLUSION_STATUS_MSG, &m_OcclusionCookie);
		if (FAILED(hr))
		{
			return ProcessFailure(m_Device, L"Failed to register for occlusion message", L"Error", hr, SystemTransitionsExpectedErrors);
		}

	}

	// Get window size
	RECT WindowRect;
	GetClientRect(m_WindowHandle, &WindowRect);
	UINT Width = WindowRect.right - WindowRect.left;
	UINT Height = WindowRect.bottom - WindowRect.top;

	// Create swapchain for window
	DXGI_SWAP_CHAIN_DESC1 SwapChainDesc;
	RtlZeroMemory(&SwapChainDesc, sizeof(SwapChainDesc));

	SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	SwapChainDesc.BufferCount = 2;
	SwapChainDesc.Width = Width;
	SwapChainDesc.Height = Height;
	SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	SwapChainDesc.SampleDesc.Count = 1;
	SwapChainDesc.SampleDesc.Quality = 0;
	hr = m_Factory->CreateSwapChainForHwnd(m_Device, Window, &SwapChainDesc, nullptr, nullptr, &m_SwapChain);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create window swapchain", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Disable the ALT-ENTER shortcut for entering full-screen mode
	hr = m_Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to make window association", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	UINT textureWidth = 0, textureHeight = 0;
	if (m_ScaledWidth != 0 && m_ScaledHeight != 0)
	{
		textureWidth = m_ScaledWidth;
		textureHeight = m_ScaledHeight;
	}
	else
	{
		textureWidth = GetOutputOptions()->GetFrameSize().cx;
		textureHeight = GetOutputOptions()->GetFrameSize().cy;
	}

	// Create shared texture for all duplication threads to draw into
	D3D11_TEXTURE2D_DESC DeskTexD;
	RtlZeroMemory(&DeskTexD, sizeof(D3D11_TEXTURE2D_DESC));
	DeskTexD.Width = textureWidth;
	DeskTexD.Height = textureHeight;
	DeskTexD.MipLevels = 1;
	DeskTexD.ArraySize = 1;
	DeskTexD.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	DeskTexD.SampleDesc.Count = 1;
	DeskTexD.Usage = D3D11_USAGE_DEFAULT;
	DeskTexD.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	DeskTexD.CPUAccessFlags = 0;
	//DeskTexD.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	hr = m_Device->CreateTexture2D(&DeskTexD, nullptr, &m_SharedSurf);

	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create shared texture", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Get keyed mutex
	/*hr = m_SharedSurf->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&m_KeyMutex));
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to query for keyed mutex in OUTPUTMANAGER", L"Error", hr);
	}*/

	// Make new render target view
	DUPL_RETURN Return = MakeRTV();
	if (Return != DUPL_RETURN_SUCCESS)
	{
		return Return;
	}

	//Calculating how much black screen should be added on either left or top
	UINT leftX = 0, topY = 0;
	float tX = static_cast<FLOAT>(Width) / static_cast<FLOAT>(DeskBounds->right - DeskBounds->left);
	float tY = static_cast<FLOAT>(Height) / static_cast<FLOAT>(DeskBounds->bottom - DeskBounds->top);

	if (tX <= tY)
	{
		topY = (Height - ((DeskBounds->bottom - DeskBounds->top) * tX)) / 2;
		Height = (DeskBounds->bottom - DeskBounds->top) * tX;
	}
	else
	{
		leftX = (Width - ((DeskBounds->right - DeskBounds->left) * tY)) / 2;
		Width = (DeskBounds->right - DeskBounds->left) * tY;
	}
	// Set view port
	SetViewPort(m_DeviceContext, Width, Height, leftX, topY);

	// Create the sample state
	D3D11_SAMPLER_DESC SampDesc;
	RtlZeroMemory(&SampDesc, sizeof(SampDesc));
	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	SampDesc.MinLOD = 0;
	SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = m_Device->CreateSamplerState(&SampDesc, &m_SamplerLinear);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create sampler state in OutputManager", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Create the blend state
	D3D11_BLEND_DESC BlendStateDesc;
	BlendStateDesc.AlphaToCoverageEnable = FALSE;
	BlendStateDesc.IndependentBlendEnable = FALSE;
	BlendStateDesc.RenderTarget[0].BlendEnable = TRUE;
	BlendStateDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	BlendStateDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	BlendStateDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	BlendStateDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	BlendStateDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	BlendStateDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	BlendStateDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = m_Device->CreateBlendState(&BlendStateDesc, &m_BlendState);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create blend state in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Initialize shaders
	hr = InitShaders(m_Device, &m_PixelShader, &m_VertexShader, &m_InputLayout);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to initialize shaders in OutputManager", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	GetWindowRect(m_WindowHandle, &WindowRect);
	//MoveWindow(m_WindowHandle, WindowRect.left, WindowRect.top, (DeskBounds->right - DeskBounds->left) / 2, (DeskBounds->bottom - DeskBounds->top) / 2, TRUE);

	return Return;
}

//
// Reset render target view
//
DUPL_RETURN OutputManager::MakeRTV()
{
	// Get backbuffer
	ID3D11Texture2D *BackBuffer = nullptr;
	HRESULT hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&BackBuffer));
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to get backbuffer for making render target view in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Create a render target view
	hr = m_Device->CreateRenderTargetView(BackBuffer, nullptr, &m_RTV);
	BackBuffer->Release();
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create render target view in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Set new render target
	m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);

	return DUPL_RETURN_SUCCESS;
}

//
// Present to the application window
//
DUPL_RETURN OutputManager::UpdateApplicationWindow(_In_ PTR_INFO *PointerInfo, _Inout_ bool *Occluded)
{
	// In a typical desktop duplication application there would be an application running on one system collecting the desktop images
	// and another application running on a different system that receives the desktop images via a network and display the image. This
	// sample contains both these aspects into a single application.
	// This routine is the part of the sample that displays the desktop image onto the display

	// Try and acquire sync on common display buffer
	//HRESULT hr = m_KeyMutex->AcquireSync(0, 100);
	//if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
	//{
	//	// Another thread has the keyed mutex so try again later
	//	return DUPL_RETURN_SUCCESS;
	//}
	//else if (FAILED(hr))
	//{
	//	return ProcessFailure(m_Device, L"Failed to acquire Keyed mutex in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	//}

	// Got mutex, so draw
	DUPL_RETURN Ret = DrawFrame();
	if (Ret == DUPL_RETURN_SUCCESS)
	{
		// We have keyed mutex so we can access the mouse info
		//if (PointerInfo != nullptr && PointerInfo->Visible)
		//{
		//	// Draw mouse into
		//	Ret = DrawMouse(PointerInfo);
		//}
	}

	// Release keyed mutex
	/*hr = m_KeyMutex->ReleaseSync(0);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to Release Keyed mutex in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}*/

	// Present to window if all worked
	if (Ret == DUPL_RETURN_SUCCESS)
	{
		// Present to window
		HRESULT hr = m_SwapChain->Present(1, 0);
		if (FAILED(hr))
		{
			return ProcessFailure(m_Device, L"Failed to present", L"Error", hr, SystemTransitionsExpectedErrors);
		}
		else if (hr == DXGI_STATUS_OCCLUDED)
		{
			*Occluded = true;
		}
	}

	return Ret;
}

//
// Draw frame into backbuffer
//
DUPL_RETURN OutputManager::DrawFrame()
{
	HRESULT hr;

	// Get window size
	RECT WindowRect;
	GetClientRect(m_WindowHandle, &WindowRect);
	UINT Width = WindowRect.right - WindowRect.left;
	UINT Height = WindowRect.bottom - WindowRect.top;

	if (PreviousWindowWidth != Width || PreviousWindowHeight != Height)
	{
		WindowResize();
	}
	else
	{
		PreviousWindowWidth = Width;
		PreviousWindowHeight = Height;
	}

	// If window was resized, resize swapchain
	if (m_NeedsResize)
	{
		DUPL_RETURN Ret = ResizeSwapChain();
		if (Ret != DUPL_RETURN_SUCCESS)
		{
			return Ret;
		}
		m_NeedsResize = false;
	}

	// Vertices for drawing whole texture
	VERTEX Vertices[NUMVERTICES] =
	{
		{XMFLOAT3(-1.0f, -1.0f, 0), XMFLOAT2(0.0f, 1.0f)},
		{XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
		{XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
		{XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
		{XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
		{XMFLOAT3(1.0f, 1.0f, 0), XMFLOAT2(1.0f, 0.0f)},
	};

	D3D11_TEXTURE2D_DESC FrameDesc;
	m_SharedSurf->GetDesc(&FrameDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC ShaderDesc;
	ShaderDesc.Format = FrameDesc.Format;
	ShaderDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	ShaderDesc.Texture2D.MostDetailedMip = FrameDesc.MipLevels - 1;
	ShaderDesc.Texture2D.MipLevels = FrameDesc.MipLevels;

	// Create new shader resource view
	ID3D11ShaderResourceView *ShaderResource = nullptr;
	hr = m_Device->CreateShaderResourceView(m_SharedSurf, &ShaderDesc, &ShaderResource);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create shader resource when drawing a frame", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	//Calculating how much black screen should be added on either left or top
	UINT leftX = 0, topY = 0;
	float tX = static_cast<FLOAT>(Width) / static_cast<FLOAT>(FrameDesc.Width);
	float tY = static_cast<FLOAT>(Height) / static_cast<FLOAT>(FrameDesc.Height);

	if (tX <= tY)
	{
		topY = (Height - ((FrameDesc.Height) * tX)) / 2;
		Height = (FrameDesc.Height) * tX;
	}
	else
	{
		leftX = (Width - ((FrameDesc.Width) * tY)) / 2;
		Width = (FrameDesc.Width) * tY;
	}
	SetViewPort(m_DeviceContext, Width, Height, leftX, topY);
	
	//hr = WriteFrameToImage(m_SharedSurf, L"D:\\test\\shared.png");
	
	// Set resources
	UINT Stride = sizeof(VERTEX);
	UINT Offset = 0;
	FLOAT blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
	m_DeviceContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
	m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);
	m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
	m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
	m_DeviceContext->PSSetShaderResources(0, 1, &ShaderResource);
	m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);
	m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	D3D11_BUFFER_DESC BufferDesc;
	RtlZeroMemory(&BufferDesc, sizeof(BufferDesc));
	BufferDesc.Usage = D3D11_USAGE_DEFAULT;
	BufferDesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
	BufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	BufferDesc.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA InitData;
	RtlZeroMemory(&InitData, sizeof(InitData));
	InitData.pSysMem = Vertices;

	ID3D11Buffer *VertexBuffer = nullptr;

	// Create vertex buffer
	hr = m_Device->CreateBuffer(&BufferDesc, &InitData, &VertexBuffer);
	if (FAILED(hr))
	{
		ShaderResource->Release();
		ShaderResource = nullptr;
		return ProcessFailure(m_Device, L"Failed to create vertex buffer when drawing a frame", L"Error", hr, SystemTransitionsExpectedErrors);
	}
	m_DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	m_DeviceContext->Draw(NUMVERTICES, 0);

	VertexBuffer->Release();
	VertexBuffer = nullptr;

	// Release shader resource
	ShaderResource->Release();
	ShaderResource = nullptr;

	return DUPL_RETURN_SUCCESS;
}

HRESULT OutputManager::WriteFrameToBuffer(_In_ ID3D11Texture2D *image, _Out_ BYTE** buffer, _Out_ long* width, _Out_ long* height)
{
	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC description;
	image->GetDesc(&description);
	(*width) = description.Width;
	(*height) = description.Height;

	description.BindFlags = 0;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
	description.Usage = D3D11_USAGE_STAGING;


	ID3D11Texture2D *imgTemp = nullptr;

	hr = m_Device->CreateTexture2D(&description, NULL, &imgTemp);
	if (FAILED(hr))
	{
		if (imgTemp)
		{
			imgTemp->Release();
			imgTemp = NULL;
		}
		*width = 0;
		*height = 0;
		return E_ACCESSDENIED;
	}
	m_DeviceContext->CopyResource(imgTemp, image);

	D3D11_MAPPED_SUBRESOURCE  mapped;
	unsigned int subresource = 0;
	hr = m_DeviceContext->Map(imgTemp, 0, D3D11_MAP_READ, 0, &mapped);

	if (FAILED(hr))
	{
		imgTemp->Release();
		imgTemp = NULL;
		*width = 0;
		*height = 0;
		return E_ACCESSDENIED;
	}

	const int pitch = mapped.RowPitch;
	BYTE *source = (BYTE *)(mapped.pData);
	BYTE *dest = new BYTE[(*width) * (*height) * 4];
	BYTE *destTemp = dest;
	for (int i = 0; i < (*height); ++i)
	{
		memcpy(destTemp, source, (*width) * 4);
		source += pitch;
		destTemp += (*width) * 4;
	}
	m_DeviceContext->Unmap(imgTemp, 0);
	imgTemp->Release();
	imgTemp = NULL;
	
	(*buffer) = dest;
	return hr;
}


//
// Resize swapchain
//
DUPL_RETURN OutputManager::ResizeSwapChain()
{
	if (m_RTV)
	{
		m_RTV->Release();
		m_RTV = nullptr;
	}

	RECT WindowRect;
	GetClientRect(m_WindowHandle, &WindowRect);
	UINT Width = WindowRect.right - WindowRect.left;
	UINT Height = WindowRect.bottom - WindowRect.top;

	// Resize swapchain
	DXGI_SWAP_CHAIN_DESC SwapChainDesc;
	m_SwapChain->GetDesc(&SwapChainDesc);
	HRESULT hr = m_SwapChain->ResizeBuffers(SwapChainDesc.BufferCount, Width, Height, SwapChainDesc.BufferDesc.Format, SwapChainDesc.Flags);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to resize swapchain buffers in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Make new render target view
	DUPL_RETURN Ret = MakeRTV();
	if (Ret != DUPL_RETURN_SUCCESS)
	{
		return Ret;
	}

	// Set new viewport
	SetViewPort(m_DeviceContext, Width, Height,0,0);

	return Ret;
}

//
// Returns shared handle
//
HANDLE OutputManager::GetSharedHandle()
{
	HANDLE Hnd = nullptr;

	// QI IDXGIResource interface to synchronized shared surface.
	IDXGIResource *DXGIResource = nullptr;
	HRESULT hr = m_SharedSurf->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&DXGIResource));
	if (SUCCEEDED(hr))
	{
		// Obtain handle to IDXGIResource object.
		DXGIResource->GetSharedHandle(&Hnd);
		DXGIResource->Release();
		DXGIResource = nullptr;
	}

	return Hnd;
}

//
// Releases all references
//
void OutputManager::CleanRefs()
{
	if (m_VertexShader)
	{
		m_VertexShader->Release();
		m_VertexShader = nullptr;
	}

	if (m_PixelShader)
	{
		m_PixelShader->Release();
		m_PixelShader = nullptr;
	}

	if (m_SamplerLinear)
	{
		m_SamplerLinear->Release();
		m_SamplerLinear = nullptr;
	}
	
	if (m_InputLayout)
	{
		m_InputLayout->Release();
		m_InputLayout = nullptr;
	}

	if (m_RTV)
	{
		m_RTV->Release();
		m_RTV = nullptr;
	}

	if (m_SwapChain)
	{
		m_SwapChain->Release();
		m_SwapChain = nullptr;
	}

	if (m_SharedSurf)
	{
		m_SharedSurf->Release();
		m_SharedSurf = nullptr;
	}

	if (m_BlendState)
	{
		m_BlendState->Release();
		m_BlendState = nullptr;
	}

	if (m_Factory)
	{
		if (m_OcclusionCookie)
		{
			m_Factory->UnregisterOcclusionStatus(m_OcclusionCookie);
			m_OcclusionCookie = 0;
		}
		m_Factory->Release();
		m_Factory = nullptr;
	}
	if (m_SinkWriter) {
		m_SinkWriter->Flush(m_VideoStreamIndex);
	}
	if (m_MediaTransform) {
		m_MediaTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
	}
}

void OutputManager::SetDeviceId(std::wstring id)
{
	device_id = id;
}

//
// Process both masked and monochrome pointers
//
DUPL_RETURN OutputManager::ProcessMonoMask(bool IsMono, _Inout_ PTR_INFO *PtrInfo, _Out_ INT *PtrWidth, _Out_ INT *PtrHeight, _Out_ INT *PtrLeft, _Out_ INT *PtrTop, _Outptr_result_bytebuffer_(*PtrHeight **PtrWidth *BPP) BYTE **InitBuffer, _Out_ D3D11_BOX *Box, _In_ ID3D11Texture2D *pBgTexture)
{
	// Desktop dimensions
	D3D11_TEXTURE2D_DESC FullDesc;
	pBgTexture->GetDesc(&FullDesc);
	INT DesktopWidth = FullDesc.Width;
	INT DesktopHeight = FullDesc.Height;

	volatile float cx = 1;
	volatile float cy = 1;
	if (!GetOutputOptions()->GetIsCustomSelectedArea())
	{
		volatile float width = static_cast<float> (GetOutputOptions()->GetScaledScreenSize().cx);
		volatile float height = static_cast<float> (GetOutputOptions()->GetScaledScreenSize().cy);
		volatile float ptrRight = static_cast<float> (PtrInfo->WhoUpdatedPositionLast.right);
		volatile float ptrBottom = static_cast<float> (PtrInfo->WhoUpdatedPositionLast.bottom);
		cx = width / ptrRight;
		cy = height / ptrBottom;
	}
	// Pointer position
	INT GivenLeft = (PtrInfo->Position.x - GetOutputOptions()->GetSourceRectangle().left) * cx;
	INT GivenTop = (PtrInfo->Position.y - GetOutputOptions()->GetSourceRectangle().top) * cy;

	// Figure out if any adjustment is needed for out of bound positions
	if (GivenLeft < 0)
	{
		*PtrWidth = GivenLeft + static_cast<INT>(PtrInfo->ShapeInfo.Width);
	}
	else if ((GivenLeft + static_cast<INT>(PtrInfo->ShapeInfo.Width)) > DesktopWidth)
	{
		*PtrWidth = DesktopWidth - GivenLeft;
	}
	else
	{
		*PtrWidth = static_cast<INT>(PtrInfo->ShapeInfo.Width);
	}

	if (IsMono)
	{
		PtrInfo->ShapeInfo.Height = PtrInfo->ShapeInfo.Height / 2;
	}

	if (GivenTop < 0)
	{
		*PtrHeight = GivenTop + static_cast<INT>(PtrInfo->ShapeInfo.Height);
	}
	else if ((GivenTop + static_cast<INT>(PtrInfo->ShapeInfo.Height)) > DesktopHeight)
	{
		*PtrHeight = DesktopHeight - GivenTop;
	}
	else
	{
		*PtrHeight = static_cast<INT>(PtrInfo->ShapeInfo.Height);
	}

	if (IsMono)
	{
		PtrInfo->ShapeInfo.Height = PtrInfo->ShapeInfo.Height * 2;
	}

	*PtrLeft = (GivenLeft < 0) ? 0 : GivenLeft;
	*PtrTop = (GivenTop < 0) ? 0 : GivenTop;

	// Staging buffer/texture
	D3D11_TEXTURE2D_DESC CopyBufferDesc;
	CopyBufferDesc.Width = *PtrWidth;
	CopyBufferDesc.Height = *PtrHeight;
	CopyBufferDesc.MipLevels = 1;
	CopyBufferDesc.ArraySize = 1;
	CopyBufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	CopyBufferDesc.SampleDesc.Count = 1;
	CopyBufferDesc.SampleDesc.Quality = 0;
	CopyBufferDesc.Usage = D3D11_USAGE_STAGING;
	CopyBufferDesc.BindFlags = 0;
	CopyBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CopyBufferDesc.MiscFlags = 0;

	ID3D11Texture2D *CopyBuffer = nullptr;
	HRESULT hr = m_Device->CreateTexture2D(&CopyBufferDesc, nullptr, &CopyBuffer);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed creating staging texture for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Copy needed part of desktop image
	Box->left = *PtrLeft;
	Box->top = *PtrTop;
	Box->right = *PtrLeft + *PtrWidth;
	Box->bottom = *PtrTop + *PtrHeight;
	m_DeviceContext->CopySubresourceRegion(CopyBuffer, 0, 0, 0, 0, pBgTexture, 0, Box);

	// QI for IDXGISurface
	IDXGISurface *CopySurface = nullptr;
	hr = CopyBuffer->QueryInterface(__uuidof(IDXGISurface), (void **)&CopySurface);
	CopyBuffer->Release();
	CopyBuffer = nullptr;
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed to QI staging texture into IDXGISurface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Map pixels
	DXGI_MAPPED_RECT MappedSurface;
	hr = CopySurface->Map(&MappedSurface, DXGI_MAP_READ);
	if (FAILED(hr))
	{
		CopySurface->Release();
		CopySurface = nullptr;
		return ProcessFailure(m_Device, L"Failed to map surface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// New mouseshape buffer
	*InitBuffer = new (std::nothrow) BYTE[*PtrWidth * *PtrHeight * BPP];
	if (!(*InitBuffer))
	{
		return ProcessFailure(nullptr, L"Failed to allocate memory for new mouse shape buffer.", L"Error", E_OUTOFMEMORY);
	}

	UINT *InitBuffer32 = reinterpret_cast<UINT *>(*InitBuffer);
	UINT *Desktop32 = reinterpret_cast<UINT *>(MappedSurface.pBits);
	UINT  DesktopPitchInPixels = MappedSurface.Pitch / sizeof(UINT);

	// What to skip (pixel offset)
	UINT SkipX = (GivenLeft < 0) ? (-1 * GivenLeft) : (0);
	UINT SkipY = (GivenTop < 0) ? (-1 * GivenTop) : (0);

	if (IsMono)
	{
		for (INT Row = 0; Row < *PtrHeight; ++Row)
		{
			// Set mask
			BYTE Mask = 0x80;
			Mask = Mask >> (SkipX % 8);
			for (INT Col = 0; Col < *PtrWidth; ++Col)
			{
				// Get masks using appropriate offsets
				BYTE AndMask = PtrInfo->PtrShapeBuffer[((Col + SkipX) / 8) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch))] & Mask;
				BYTE XorMask = PtrInfo->PtrShapeBuffer[((Col + SkipX) / 8) + ((Row + SkipY + (PtrInfo->ShapeInfo.Height / 2)) * (PtrInfo->ShapeInfo.Pitch))] & Mask;
				UINT AndMask32 = (AndMask) ? 0xFFFFFFFF : 0xFF000000;
				UINT XorMask32 = (XorMask) ? 0x00FFFFFF : 0x00000000;

				// Set new pixel
				InitBuffer32[(Row * *PtrWidth) + Col] = (Desktop32[(Row * DesktopPitchInPixels) + Col] & AndMask32) ^ XorMask32;

				// Adjust mask
				if (Mask == 0x01)
				{
					Mask = 0x80;
				}
				else
				{
					Mask = Mask >> 1;
				}
			}
		}
	}
	else
	{
		UINT *Buffer32 = reinterpret_cast<UINT *>(PtrInfo->PtrShapeBuffer);

		// Iterate through pixels
		for (INT Row = 0; Row < *PtrHeight; ++Row)
		{
			for (INT Col = 0; Col < *PtrWidth; ++Col)
			{
				// Set up mask
				UINT MaskVal = 0xFF000000 & Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))];
				if (MaskVal)
				{
					// Mask was 0xFF
					InitBuffer32[(Row * *PtrWidth) + Col] = (Desktop32[(Row * DesktopPitchInPixels) + Col] ^ Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))]) | 0xFF000000;
				}
				else
				{
					// Mask was 0x00
					InitBuffer32[(Row * *PtrWidth) + Col] = Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))] | 0xFF000000;
				}
			}
		}
	}

	// Done with resource
	hr = CopySurface->Unmap();
	CopySurface->Release();
	CopySurface = nullptr;
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to unmap surface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	return DUPL_RETURN_SUCCESS;
}

//
// Draw mouse provided in buffer to backbuffer
//
DUPL_RETURN OutputManager::DrawMouse(_In_ PTR_INFO *PtrInfo, _Inout_ ID3D11Texture2D *pBgTexture)
{
	// Vars to be used
	ID3D11Texture2D *MouseTex = nullptr;
	ID3D11ShaderResourceView *ShaderRes = nullptr;
	ID3D11Buffer *VertexBufferMouse = nullptr;
	D3D11_SUBRESOURCE_DATA InitData;
	D3D11_TEXTURE2D_DESC Desc;
	D3D11_SHADER_RESOURCE_VIEW_DESC SDesc;

	// Position will be changed based on mouse position
	VERTEX Vertices[NUMVERTICES] =
	{
		{XMFLOAT3(-1.0f, -1.0f, 0), XMFLOAT2(0.0f, 1.0f)},
		{XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
		{XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
		{XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
		{XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
		{XMFLOAT3(1.0f, 1.0f, 0), XMFLOAT2(1.0f, 0.0f)},
	};

	D3D11_TEXTURE2D_DESC FullDesc;
	pBgTexture->GetDesc(&FullDesc);
	INT DesktopWidth = FullDesc.Width;
	INT DesktopHeight = FullDesc.Height;

	// Center of desktop dimensions
	INT CenterX = (DesktopWidth / 2);
	INT CenterY = (DesktopHeight / 2);

	// Clipping adjusted coordinates / dimensions
	INT PtrWidth = 0;
	INT PtrHeight = 0;
	INT PtrLeft = 0;
	INT PtrTop = 0;

	// Buffer used if necessary (in case of monochrome or masked pointer)
	BYTE *InitBuffer = nullptr;

	// Used for copying pixels
	D3D11_BOX Box;
	Box.front = 0;
	Box.back = 1;

	Desc.MipLevels = 1;
	Desc.ArraySize = 1;
	Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Desc.SampleDesc.Count = 1;
	Desc.SampleDesc.Quality = 0;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	Desc.CPUAccessFlags = 0;
	Desc.MiscFlags = 0;

	// Set shader resource properties
	SDesc.Format = Desc.Format;
	SDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	SDesc.Texture2D.MostDetailedMip = Desc.MipLevels - 1;
	SDesc.Texture2D.MipLevels = Desc.MipLevels;

	volatile float cx = 1;
	volatile float cy = 1;
	if (!GetOutputOptions()->GetIsCustomSelectedArea())
	{
		volatile float width = static_cast<float> (GetOutputOptions()->GetScaledScreenSize().cx);
		volatile float height = static_cast<float> (GetOutputOptions()->GetScaledScreenSize().cy);
		volatile float ptrRight = static_cast<float> (PtrInfo->WhoUpdatedPositionLast.right);
		volatile float ptrBottom = static_cast<float> (PtrInfo->WhoUpdatedPositionLast.bottom);
		cx = width / ptrRight;
		cy = height / ptrBottom;
	}
	
	switch (PtrInfo->ShapeInfo.Type)
	{
		case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
		{
			PtrLeft = (PtrInfo->Position.x - GetOutputOptions()->GetSourceRectangle().left) * cx;
			PtrTop = (PtrInfo->Position.y - GetOutputOptions()->GetSourceRectangle().top) * cy;

			PtrWidth = static_cast<INT>(PtrInfo->ShapeInfo.Width) * cx;
			PtrHeight = static_cast<INT>(PtrInfo->ShapeInfo.Height) * cy;

			break;
		}

		case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
		{
			ProcessMonoMask(true, PtrInfo, &PtrWidth, &PtrHeight, &PtrLeft, &PtrTop, &InitBuffer, &Box, pBgTexture);
			break;
		}

		case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
		{
			ProcessMonoMask(false, PtrInfo, &PtrWidth, &PtrHeight, &PtrLeft, &PtrTop, &InitBuffer, &Box, pBgTexture);
			break;
		}

		default:
			break;
	}

	// VERTEX creation
	Vertices[0].Pos.x = (PtrLeft - CenterX) / (FLOAT)CenterX;
	Vertices[0].Pos.y = -1 * ((PtrTop + PtrHeight) - CenterY) / (FLOAT)CenterY;
	Vertices[1].Pos.x = (PtrLeft - CenterX) / (FLOAT)CenterX;
	Vertices[1].Pos.y = -1 * (PtrTop - CenterY) / (FLOAT)CenterY;
	Vertices[2].Pos.x = ((PtrLeft + PtrWidth) - CenterX) / (FLOAT)CenterX;
	Vertices[2].Pos.y = -1 * ((PtrTop + PtrHeight) - CenterY) / (FLOAT)CenterY;
	Vertices[3].Pos.x = Vertices[2].Pos.x;
	Vertices[3].Pos.y = Vertices[2].Pos.y;
	Vertices[4].Pos.x = Vertices[1].Pos.x;
	Vertices[4].Pos.y = Vertices[1].Pos.y;
	Vertices[5].Pos.x = ((PtrLeft + PtrWidth) - CenterX) / (FLOAT)CenterX;
	Vertices[5].Pos.y = -1 * (PtrTop - CenterY) / (FLOAT)CenterY;

	// Set texture properties
	Desc.Width = PtrWidth;
	Desc.Height = PtrHeight;

	// Set up init data
	InitData.pSysMem = (PtrInfo->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? PtrInfo->PtrShapeBuffer : InitBuffer;
	InitData.SysMemPitch = (PtrInfo->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? PtrInfo->ShapeInfo.Pitch : PtrWidth * BPP;
	InitData.SysMemSlicePitch = 0;

	// Create mouseshape as texture
	HRESULT hr = m_Device->CreateTexture2D(&Desc, &InitData, &MouseTex);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create mouse pointer texture", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Create shader resource from texture
	hr = m_Device->CreateShaderResourceView(MouseTex, &SDesc, &ShaderRes);
	if (FAILED(hr))
	{
		MouseTex->Release();
		MouseTex = nullptr;
		return ProcessFailure(m_Device, L"Failed to create shader resource from mouse pointer texture", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	D3D11_BUFFER_DESC BDesc;
	ZeroMemory(&BDesc, sizeof(D3D11_BUFFER_DESC));
	BDesc.Usage = D3D11_USAGE_DEFAULT;
	BDesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
	BDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	BDesc.CPUAccessFlags = 0;

	ZeroMemory(&InitData, sizeof(D3D11_SUBRESOURCE_DATA));
	InitData.pSysMem = Vertices;

	// Create vertex buffer
	hr = m_Device->CreateBuffer(&BDesc, &InitData, &VertexBufferMouse);
	if (FAILED(hr))
	{
		ShaderRes->Release();
		ShaderRes = nullptr;
		MouseTex->Release();
		MouseTex = nullptr;
		return ProcessFailure(m_Device, L"Failed to create mouse pointer vertex buffer in OutputManager", L"Error", hr, SystemTransitionsExpectedErrors);
	}
	ID3D11RenderTargetView *RTV;
	// Create a render target view
	hr = m_Device->CreateRenderTargetView(pBgTexture, nullptr, &RTV);
	// Set resources
	FLOAT BlendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
	UINT Stride = sizeof(VERTEX);
	UINT Offset = 0;
	m_DeviceContext->IASetVertexBuffers(0, 1, &VertexBufferMouse, &Stride, &Offset);
	m_DeviceContext->OMSetBlendState(m_BlendState, BlendFactor, 0xFFFFFFFF);
	m_DeviceContext->OMSetRenderTargets(1, &RTV, nullptr);
	m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
	m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
	m_DeviceContext->PSSetShaderResources(0, 1, &ShaderRes);
	m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);

	// Save current view port so we can restore later
	D3D11_VIEWPORT VP;
	UINT numViewports = 1;
	m_DeviceContext->RSGetViewports(&numViewports, &VP);

	// Draw
	if (!GetOutputOptions()->GetIsCustomSelectedArea())
	{
		SetViewPort(m_DeviceContext, static_cast<float>(GetOutputOptions()->GetScaledScreenSize().cx), static_cast<float>(GetOutputOptions()->GetScaledScreenSize().cy));
	}
	else
	{
		SetViewPort(m_DeviceContext, static_cast<float>(GetOutputOptions()->GetFrameSize().cx), static_cast<float>(GetOutputOptions()->GetFrameSize().cy));
	}
	m_DeviceContext->Draw(NUMVERTICES, 0);

	// Restore view port
	m_DeviceContext->RSSetViewports(1, &VP);
	// Clean
	if (RTV) {
		RTV->Release();
		RTV = nullptr;
	}
	ID3D11ShaderResourceView *null[] = { nullptr, nullptr };
	m_DeviceContext->PSSetShaderResources(0, 1, null);
	if (VertexBufferMouse)
	{
		VertexBufferMouse->Release();
		VertexBufferMouse = nullptr;
	}
	if (ShaderRes)
	{
		ShaderRes->Release();
		ShaderRes = nullptr;
	}
	if (MouseTex)
	{
		MouseTex->Release();
		MouseTex = nullptr;
	}
	if (InitBuffer)
	{
		delete[] InitBuffer;
		InitBuffer = nullptr;
	}

	return DUPL_RETURN_SUCCESS;
}

void OutputManager::SetMousePtrInfo(_In_ PTR_INFO *PtrInfo)
{
	m_PtrInfo = PtrInfo;
}