#pragma once
#include "VideoCamLib.h"
#include "Log.h"

class DshowCapture;

typedef void(__stdcall *PFN_CaptureCallback)(DWORD dwSize, BYTE *pbData);
typedef void(__stdcall DshowCapture::*LPFN_CaptureCallback)(DWORD dwSize, BYTE *pbData);

class CaptureCallbackBase
{
public:
    // input: pointer to a unique C callback. 
    CaptureCallbackBase(PFN_CaptureCallback pCCallback)
        : m_pClass(NULL),
        m_pMethod(NULL),
        m_pCCallback(pCCallback)
    {
    }

    // when done, remove allocation of the callback
    void Free()
    {
        m_pClass = NULL;
        m_pMethod = NULL;
    }

    // when free, allocate this callback
    PFN_CaptureCallback Reserve(DshowCapture *instance, LPFN_CaptureCallback method)
    {
        if (m_pClass) 
        {
            return NULL;
        }

        m_pClass = instance;
        m_pMethod = method;

        return m_pCCallback;
    }

protected:
    static void StaticInvoke(int context, DWORD dwSize, BYTE *pbData);

private:
    PFN_CaptureCallback m_pCCallback;
    DshowCapture *m_pClass;
    LPFN_CaptureCallback m_pMethod;
};

template <int context> class DynamicCaptureCallback : public CaptureCallbackBase
{
public:
    DynamicCaptureCallback()
        : CaptureCallbackBase(&DynamicCaptureCallback<context>::GeneratedCaptureCallback)
    {
    }

private:
    static void __stdcall GeneratedCaptureCallback(DWORD dwSize, BYTE *pbData)
    {
        return StaticInvoke(context, dwSize, pbData);
    }
};

class CaptureMemberFunctionCallback
{
public:
    CaptureMemberFunctionCallback(DshowCapture *instance, LPFN_CaptureCallback method);
    ~CaptureMemberFunctionCallback();

public:
    operator PFN_CaptureCallback() const
    {
        return m_cbCallback;
    }

    bool IsValid() const
    {
        return m_cbCallback != nullptr;
    }

private:
    PFN_CaptureCallback m_cbCallback;
    int m_nAllocIndex;
private:
    CaptureMemberFunctionCallback(const CaptureMemberFunctionCallback &os);
    CaptureMemberFunctionCallback& operator= (const CaptureMemberFunctionCallback &os); 
    //CallbackMemberFunctionCallback& (reference?) gets returned by the assignment operator =
    //this lets you call x1 = x2 = CaptureMemberFunctionCallback of a static?
    // means x2 = static function; x1 = x2;
};

