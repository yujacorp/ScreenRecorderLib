#include "CameraCapture.h"
#include "Cleanup.h"

CameraCapture::CameraCapture() :SourceReaderBase()
{
}

CameraCapture::~CameraCapture()
{
}

HRESULT CameraCapture::InitializeSourceReader(
	_In_ std::wstring source,
	_In_ std::optional<SIZE> outputSize,
	_In_ std::optional<long> sourceFormatIndex,
	_Out_ long *pStreamIndex,
	_Outptr_ IMFSourceReader **ppSourceReader,
	_Outptr_ IMFMediaType **ppInputMediaType,
	_Outptr_opt_ IMFMediaType **ppOutputMediaType,
	_Outptr_opt_result_maybenull_ IMFTransform **ppMediaTransform)
{
	MeasureExecutionTime measure(L"InitializeSourceReader");
	if (ppSourceReader) {
		*ppSourceReader = nullptr;
	}
	if (ppInputMediaType) {
		*ppInputMediaType = nullptr;
	}
	if (ppOutputMediaType) {
		*ppOutputMediaType = nullptr;
	}
	if (ppMediaTransform) {
		*ppMediaTransform = nullptr;
	}
	*pStreamIndex = 0;
	HRESULT hr = S_OK;
	CComPtr<IMFAttributes> pAttributes = nullptr;
	CComPtr<IMFSourceReader> pSourceReader = nullptr;
	EnterCriticalSection(&m_CriticalSection);
	LeaveCriticalSectionOnExit leaveCriticalSection(&m_CriticalSection, L"InitializeSourceReader device capture");
	UINT32 count = 0;
	IMFActivate **ppDevices = NULL;
	ReleaseCOMArrayOnExit releaseDevicesOnExit((IUnknown **)ppDevices, count);
	// Create an attribute store to specify enumeration parameters.
	RETURN_ON_BAD_HR(hr = MFCreateAttributes(&pAttributes, 1));

	//The attribute to be requested is devices that can capture video
	RETURN_ON_BAD_HR(hr = pAttributes->SetGUID(
		MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
		MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
	));
	{
		MeasureExecutionTime measure(L"MFEnumDeviceSources");
		//Enummerate the video capture devices
		RETURN_ON_BAD_HR(hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count));
		if (count == 0) {
			LOG_ERROR("No video capture devices found");
			hr = E_FAIL;
		}
	}
	// Try to find a suitable output type.
	for (UINT32 i = 0; i < count; i++)
	{
		WCHAR *symbolicLink = NULL;
		UINT32 cchSymbolicLink;
		IMFActivate *pDevice = ppDevices[i];
		hr = pDevice->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &cchSymbolicLink);
		CoTaskMemFreeOnExit freeSymbolicLink(symbolicLink);
		if (symbolicLink == NULL || !source.empty() && source != symbolicLink) {
			continue;
		}
		m_DeviceSymbolicLink = std::wstring(symbolicLink);
		LOG_TRACE(L"Try to activate media source");
		CComPtr<IMFMediaSource> pSource = nullptr;
		CONTINUE_ON_BAD_HR(hr = pDevice->ActivateObject(__uuidof(IMFMediaSource), (void **)&pSource));
		LOG_TRACE(L"Try to initialize media source");
		CONTINUE_ON_BAD_HR(hr = InitializeMediaSource(pSource, outputSize, sourceFormatIndex, pStreamIndex, ppSourceReader, ppInputMediaType, ppOutputMediaType, ppMediaTransform));

		WCHAR *nameString = NULL;
		// Get the human-friendly name of the device
		UINT32 cchName;
		hr = pDevice->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
			&nameString, &cchName);

		if (SUCCEEDED(hr) && nameString != NULL)
		{
			//allocate a byte buffer for the raw pixel data
			m_DeviceName = std::wstring(nameString);
		}
		CoTaskMemFree(nameString);
	}
	if (FAILED(hr))
	{
		Close();
	}
	return hr;
}

HRESULT CameraCapture::InitializeMediaSource(
	_In_ CComPtr<IMFMediaSource> pSource,
	_In_ std::optional<SIZE> outputSize,
	_In_ std::optional<long> sourceFormatIndex,
	_Out_ long *pStreamIndex,
	_Outptr_ IMFSourceReader **ppSourceReader,
	_Outptr_ IMFMediaType **ppInputMediaType,
	_Outptr_opt_ IMFMediaType **ppOutputMediaType,
	_Outptr_opt_result_maybenull_ IMFTransform **ppMediaTransform
)
{
	MeasureExecutionTime measure(L"InitializeMediaSource");
	CComPtr<IMFSourceReader> pSourceReader = nullptr;
	CComPtr<IMFAttributes> pAttributes = nullptr;
	EnterCriticalSection(&m_CriticalSection);
	LeaveCriticalSectionOnExit leaveCriticalSection(&m_CriticalSection);

	if (sourceFormatIndex.has_value()) {
		SetDeviceFormat(pSource, sourceFormatIndex.value());
	}
	//Allocate attributes
	HRESULT hr = MFCreateAttributes(&pAttributes, 2);
	//get attributes
	if (SUCCEEDED(hr))
		hr = pAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
	// Set the callback pointer.
	if (SUCCEEDED(hr))
		hr = pAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this);
	//Create the source reader
	if (SUCCEEDED(hr))
		hr = MFCreateSourceReaderFromMediaSource(pSource, pAttributes, &pSourceReader);

	bool noMoreMedia = false;
	bool foundValidTopology = false;
	// Try to find a suitable output type.
	if (SUCCEEDED(hr))
	{
		GUID subTypeList[] = { MFVideoFormat_NV12, MFVideoFormat_MJPG };
		int subTypeIndex = 0;
		bool useAlternateFormat = false;
		bool useDefaultResolution = false;

		for (DWORD streamIndex = 0; ; streamIndex++)
		{
			for (DWORD mediaTypeIndex = sourceFormatIndex.value_or(0); ; mediaTypeIndex++)
			{
				CComPtr<IMFMediaType> pInputMediaType = nullptr;
				CComPtr<IMFMediaType> pOutputMediaType = nullptr;
				CComPtr<IMFTransform> pMediaTransform = nullptr;
				hr = pSourceReader->GetNativeMediaType(streamIndex, mediaTypeIndex, &pInputMediaType);
				if (FAILED(hr))
				{
					if (subTypeIndex < 1)
					{
						// Couldn't find NV12 format. Try with MJPG format
						subTypeIndex++;
						mediaTypeIndex = -1;
						continue;
					}
					else if (subTypeIndex == 1)
					{
						// Couldn't find NV12 and MJPG format.
						subTypeIndex++;
						useAlternateFormat = true;
						mediaTypeIndex = -1;
						continue;
					}
					
					if (subTypeIndex == 2 && !useDefaultResolution)
					{
						// Couldn't find NV12, MJPG format and default format. Most likely resolution didn't match. Use default resolution and start from NV12 format again.
						subTypeIndex = 0;
						useAlternateFormat = false;
						useDefaultResolution = true;
						mediaTypeIndex = -1;
						continue;
					}

					noMoreMedia = true;
					break;
				}

				GUID inputMajorType;
				pInputMediaType->GetGUID(MF_MT_MAJOR_TYPE, &inputMajorType);
				if (inputMajorType != MFMediaType_Video) {
					continue;
				}
				GUID inputSubType;
				pInputMediaType->GetGUID(MF_MT_SUBTYPE, &inputSubType);

				if (!useAlternateFormat && subTypeList[subTypeIndex] != inputSubType)
				{
					continue;
				}

				UINT32 inputWidth;
				UINT32 inputHeight;
				CONTINUE_ON_BAD_HR(MFGetAttributeSize(pInputMediaType, MF_MT_FRAME_SIZE, &inputWidth, &inputHeight));

				if (!useDefaultResolution && outputSize.has_value())
				{
					if (outputSize.value().cx != inputWidth || outputSize.value().cy != inputHeight)
					{
						continue;
					}
				}


				LogMediaType(pInputMediaType);
				if (ppOutputMediaType) {
					SafeRelease(&pMediaTransform);
					CONTINUE_ON_BAD_HR(pSourceReader->SetCurrentMediaType(streamIndex, NULL, pInputMediaType));
					hr = CreateIMFTransform(streamIndex, pInputMediaType, &pMediaTransform, &pOutputMediaType);
					if (FAILED(hr)) {
						LOG_INFO("Failed to create a valid media output type for video reader, attempting to create an intermediate transform");
						CComPtr<IMFActivate> pConverterActivate = NULL;
						CONTINUE_ON_BAD_HR(hr = FindVideoDecoder(&inputSubType, nullptr, false, true, true, &pConverterActivate));
						CComPtr<IMFTransform> pConverter = NULL;
						CONTINUE_ON_BAD_HR(pConverterActivate->ActivateObject(IID_PPV_ARGS(&pConverter)));
						CONTINUE_ON_BAD_HR(pConverter->SetInputType(streamIndex, pInputMediaType, 0));
						GUID guidMinor;
						GUID guidMajor;
						for (int i = 0;; i++)
						{
							SafeRelease(&pMediaTransform);
							IMFMediaType *mediaType;
							hr = pConverter->GetOutputAvailableType(streamIndex, i, &mediaType);
							if (FAILED(hr))
							{
								break;
							}
							hr = mediaType->GetGUID(MF_MT_MAJOR_TYPE, &guidMajor);
							if (guidMajor == MFMediaType_Video) {
								hr = mediaType->GetGUID(MF_MT_SUBTYPE, &guidMinor);
								IMFMediaType *pIntermediateMediaType;
								// Define the output type.
								CONTINUE_ON_BAD_HR(hr = MFCreateMediaType(&pIntermediateMediaType));
								CONTINUE_ON_BAD_HR(hr = pInputMediaType->CopyAllItems(pIntermediateMediaType));
								CONTINUE_ON_BAD_HR(hr = pIntermediateMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
								CONTINUE_ON_BAD_HR(hr = pIntermediateMediaType->SetGUID(MF_MT_SUBTYPE, guidMinor));
								CONTINUE_ON_BAD_HR(hr = pSourceReader->SetCurrentMediaType(streamIndex, NULL, pIntermediateMediaType));
								CONTINUE_ON_BAD_HR(hr = CreateIMFTransform(streamIndex, pIntermediateMediaType, &pMediaTransform, &pOutputMediaType));
								LOG_DEBUG("Successfully created video reader intermediate media transform:");
								LogMediaType(pIntermediateMediaType);
								break;
							}
						}
						pConverterActivate->ShutdownObject();
					}
				}
				if (SUCCEEDED(hr)) {// Found an output type.
					if (ppInputMediaType) {
						*ppInputMediaType = pInputMediaType;
						(*ppInputMediaType)->AddRef();
					}
					if (ppOutputMediaType) {
						*ppOutputMediaType = pOutputMediaType;
						(*ppOutputMediaType)->AddRef();
					}
					if (ppSourceReader) {
						*ppSourceReader = pSourceReader;
						(*ppSourceReader)->AddRef();
					}
					if (ppMediaTransform) {
						*ppMediaTransform = pMediaTransform;
						(*ppMediaTransform)->AddRef();
					}
					*pStreamIndex = streamIndex;
					foundValidTopology = true;
					break;
				}
				else {
					LOG_ERROR("Failed to create topology for video reader");
				}
			}
			if (noMoreMedia || foundValidTopology) {
				break;
			}
		}
	}

	if (FAILED(hr))
	{
		if (pSource)
		{
			pSource->Shutdown();
		}
		Close();
	}
	return hr;
}

HRESULT CameraCapture::SetDeviceFormat(_In_ CComPtr<IMFMediaSource> pDevice, _In_ DWORD dwFormatIndex)
{
	IMFPresentationDescriptor *pPD = NULL;
	IMFStreamDescriptor *pSD = NULL;
	IMFMediaTypeHandler *pHandler = NULL;
	IMFMediaType *pType = NULL;

	HRESULT hr = pDevice->CreatePresentationDescriptor(&pPD);
	if (FAILED(hr))
	{
		goto done;
	}

	BOOL fSelected;
	hr = pPD->GetStreamDescriptorByIndex(0, &fSelected, &pSD);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = pSD->GetMediaTypeHandler(&pHandler);
	if (FAILED(hr))
	{
		goto done;
	}
	hr = pHandler->GetMediaTypeByIndex(dwFormatIndex, &pType);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = pHandler->SetCurrentMediaType(pType);

done:
	SafeRelease(&pPD);
	SafeRelease(&pSD);
	SafeRelease(&pHandler);
	SafeRelease(&pType);
	return hr;
}