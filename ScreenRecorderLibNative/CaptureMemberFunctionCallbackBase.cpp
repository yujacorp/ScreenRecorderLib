#include "CaptureMemberFunctionCallbackBase.h"

static SRWLOCK g_lock = { SRWLOCK_INIT };

static CaptureCallbackBase *AvailableCallbackSlots[] = {
	new DynamicCaptureCallback<0x00>(),
	new DynamicCaptureCallback<0x01>(),
	new DynamicCaptureCallback<0x02>(),
	new DynamicCaptureCallback<0x03>(),
	new DynamicCaptureCallback<0x04>(),
	new DynamicCaptureCallback<0x05>(),
	//new DynamicCaptureCallback<0x06>(),
	//new DynamicCaptureCallback<0x07>(),
	//new DynamicCaptureCallback<0x08>(),
	//new DynamicCaptureCallback<0x09>(),
	//new DynamicCaptureCallback<0x0A>(),
	//new DynamicCaptureCallback<0x0B>(),
	//new DynamicCaptureCallback<0x0C>(),
	//new DynamicCaptureCallback<0x0D>(),
	//new DynamicCaptureCallback<0x0E>(),
	//new DynamicCaptureCallback<0x0F>(),
};

void CaptureCallbackBase::StaticInvoke(int context, DWORD dwSize, BYTE *pbData)
{
	bool result = TryAcquireSRWLockShared(&g_lock);
	if (!result) 
	{
		return;
	}
	CaptureCallbackBase* base = AvailableCallbackSlots[context];
	DshowCapture *sbase = base->m_pClass;
	//sbase can be null?
	LPFN_CaptureCallback method = base->m_pMethod;
	if (sbase != nullptr && method != nullptr)
	{
		(sbase->*method)(dwSize, pbData);
	}
	else 
	{
		LOG_ERROR("This shouldn't happen in StaticInvoke");
	}
	ReleaseSRWLockShared(&g_lock);
}

CaptureMemberFunctionCallback::CaptureMemberFunctionCallback(DshowCapture *instance, LPFN_CaptureCallback method)
{
	AcquireSRWLockExclusive(&g_lock);

	int imax = sizeof(AvailableCallbackSlots) / sizeof(AvailableCallbackSlots[0]);
	
	for (int i = 0; i < imax; ++i)
	{
		m_cbCallback = AvailableCallbackSlots[i]->Reserve(instance, method);
		if (m_cbCallback != NULL) 
		{
			m_nAllocIndex = i;
			break;
		}
			
	}

	ReleaseSRWLockExclusive(&g_lock);
}

CaptureMemberFunctionCallback::~CaptureMemberFunctionCallback()
{
	AcquireSRWLockExclusive(&g_lock);
	AvailableCallbackSlots[m_nAllocIndex]->Free();
	ReleaseSRWLockExclusive(&g_lock);
}