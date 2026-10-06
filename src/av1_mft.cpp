// av1_mft.cpp - in-process AV1 decoder MFT backed by dav1d.
// Local MFTs outrank registry MFTs, which is the whole reason this works.
// See docs/TECHNICAL.md.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <dav1d/dav1d.h>

#include <deque>
#include <vector>
#include <new>
#include <string.h>
#include <stdio.h>

void Av1MftLog(const char *fmt, ...);

// MinGW headers don't always define this (FOURCC 'AV01')
static const GUID AV1_FMT = {
    0x31305641, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#ifndef MFT_ENUM_HARDWARE_URL_Attribute
#define MFT_ENUM_HARDWARE_URL_Attribute MF_ATTRIBUTE_GUID
#endif

template <class T>
static void SafeRelease(T **pp) {
    if (*pp) { (*pp)->Release(); *pp = NULL; }
}

static DWORD GetFrameStride(UINT32 width) {
    return (DWORD)(((width + 15) & ~15u));
}

class Av1DecMFT : public IMFTransform {
  public:
    Av1DecMFT();
    virtual ~Av1DecMFT();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetStreamLimits(DWORD *inMin, DWORD *inMax, DWORD *outMin,
                                 DWORD *outMax) override;
    STDMETHODIMP GetStreamCount(DWORD *inCount, DWORD *outCount) override;
    STDMETHODIMP GetStreamIDs(DWORD, DWORD *, DWORD, DWORD *) override;
    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO *info) override;
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO *info) override;
    STDMETHODIMP GetAttributes(IMFAttributes **attr) override;
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes **) override;
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes **) override;
    STDMETHODIMP DeleteInputStream(DWORD) override;
    STDMETHODIMP AddInputStreams(DWORD, DWORD *) override;
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD index,
                                       IMFMediaType **type) override;
    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD index,
                                        IMFMediaType **type) override;
    STDMETHODIMP SetInputType(DWORD id, IMFMediaType *type, DWORD flags) override;
    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType *type, DWORD flags) override;
    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType **type) override;
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType **type) override;
    STDMETHODIMP GetInputStatus(DWORD id, DWORD *flags) override;
    STDMETHODIMP GetOutputStatus(DWORD *flags) override;
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override;
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent *) override;
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE msg, ULONG_PTR param) override;
    STDMETHODIMP ProcessInput(DWORD id, IMFSample *sample, DWORD flags) override;
    STDMETHODIMP ProcessOutput(DWORD flags, DWORD count,
                               MFT_OUTPUT_DATA_BUFFER *buffers,
                               DWORD *status) override;

  private:
    bool EnsureDecoder();
    bool PumpDecoder();
    bool ConvertToNV12(Dav1dPicture *pic, IMFSample *out);
    void Reset();

    LONG m_ref;
    IMFMediaType *m_inType;
    IMFMediaType *m_outType;
    IMFAttributes *m_attrs;

    UINT32 m_width, m_height;
    bool m_ready;

    Dav1dContext *m_ctx;
    Dav1dSettings m_settings;

    std::vector<unsigned char> m_obuCache;
    bool m_obuFed;

    std::deque<std::vector<unsigned char> > m_pending;
    std::deque<LONGLONG> m_pendingTime;
    std::vector<unsigned char> m_inFlight;   // EAGAIN handed this back; keep the bytes alive
    bool m_hasInFlight;
    LONGLONG m_inFlightTime;

    std::deque<LONGLONG> m_inTimeQueue;      // times of accepted input, one per output frame
    std::deque<Dav1dPicture> m_frames;
    LONGLONG m_frameDuration;                // 100ns units
    long m_framesOut;
    LONGLONG m_lastTime;
};

Av1DecMFT::Av1DecMFT()
    : m_ref(1), m_inType(NULL), m_outType(NULL), m_attrs(NULL), m_width(0),
      m_height(0), m_ready(false), m_ctx(NULL), m_obuFed(false),
      m_hasInFlight(false), m_inFlightTime(0), m_frameDuration(166667),
      m_framesOut(0), m_lastTime(0) {
    dav1d_default_settings(&m_settings);
    m_settings.n_threads = 0;          // 0 = auto
    m_settings.max_frame_delay = 1;
    m_settings.all_layers = 1;
    if (FAILED(MFCreateAttributes(&m_attrs, 4))) m_attrs = NULL;
    else {
        m_attrs->SetUINT32(MF_TRANSFORM_FLAGS_Attribute, 0);
    }
    Av1MftLog("Av1DecMFT instance created");
}

Av1DecMFT::~Av1DecMFT() {
    Reset();
    if (m_ctx) dav1d_close(&m_ctx);
    SafeRelease(&m_inType);
    SafeRelease(&m_outType);
    SafeRelease(&m_attrs);
    Av1MftLog("Av1DecMFT instance destroyed");
}

void Av1DecMFT::Reset() {
    for (size_t i = 0; i < m_frames.size(); ++i) dav1d_picture_unref(&m_frames[i]);
    m_frames.clear();
    m_pending.clear();
    m_pendingTime.clear();
    m_inFlight.clear();
    m_hasInFlight = false;
    m_inFlightTime = 0;
    m_inTimeQueue.clear();
    if (m_ctx) { dav1d_close(&m_ctx); }
    m_ready = false;
    m_obuFed = false;
}

STDMETHODIMP Av1DecMFT::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (riid == IID_IUnknown || riid == IID_IMFTransform) {
        *ppv = static_cast<IMFTransform *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) Av1DecMFT::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) Av1DecMFT::Release() {
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP Av1DecMFT::GetStreamLimits(DWORD *inMin, DWORD *inMax, DWORD *outMin,
                                        DWORD *outMax) {
    if (inMin) *inMin = 1;
    if (inMax) *inMax = 1;
    if (outMin) *outMin = 1;
    if (outMax) *outMax = 1;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::GetStreamCount(DWORD *inCount, DWORD *outCount) {
    if (!inCount || !outCount) return E_POINTER;
    *inCount = 1;
    *outCount = 1;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::GetStreamIDs(DWORD, DWORD *, DWORD, DWORD *) {
    return E_NOTIMPL;
}

STDMETHODIMP Av1DecMFT::GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO *info) {
    if (!info) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    info->hnsMaxLatency = 0;
    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES |
                    MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                    MFT_INPUT_STREAM_FIXED_SAMPLE_SIZE;
    info->cbSize = 0;
    info->cbMaxLookahead = 0;
    info->cbAlignment = 0;
    return S_OK;
}

STDMETHODIMP Av1DecMFT::GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO *info) {
    if (!info) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES |
                    MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                    MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE;
    info->cbSize = m_width && m_height
                       ? GetFrameStride(m_width) * m_height * 3 / 2
                       : 0;
    info->cbAlignment = 0;
    return S_OK;
}

STDMETHODIMP Av1DecMFT::GetAttributes(IMFAttributes **attr) {
    if (!attr) return E_POINTER;
    if (!m_attrs) return E_NOTIMPL;
    m_attrs->AddRef();
    *attr = m_attrs;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::GetInputStreamAttributes(DWORD, IMFAttributes **) {
    return E_NOTIMPL;
}
STDMETHODIMP Av1DecMFT::GetOutputStreamAttributes(DWORD, IMFAttributes **) {
    return E_NOTIMPL;
}
STDMETHODIMP Av1DecMFT::DeleteInputStream(DWORD) { return E_NOTIMPL; }
STDMETHODIMP Av1DecMFT::AddInputStreams(DWORD, DWORD *) { return E_NOTIMPL; }

STDMETHODIMP Av1DecMFT::GetInputAvailableType(DWORD id, DWORD index,
                                              IMFMediaType **type) {
    if (!type) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (index != 0) return MF_E_NO_MORE_TYPES;

    IMFMediaType *mt = NULL;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, AV1_FMT);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    *type = mt;
    return S_OK;
}

STDMETHODIMP Av1DecMFT::GetOutputAvailableType(DWORD id, DWORD index,
                                               IMFMediaType **type) {
    if (!type) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (index != 0) return MF_E_NO_MORE_TYPES;
    if (!m_inType) return MF_E_TRANSFORM_TYPE_NOT_SET;

    IMFMediaType *mt = NULL;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(m_inType, MF_MT_FRAME_SIZE, &w, &h);

    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, w, h);
    mt->SetUINT32(MF_MT_DEFAULT_STRIDE, GetFrameStride(w));
    mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    mt->SetUINT32(MF_MT_SAMPLE_SIZE, GetFrameStride(w) * h * 3 / 2);

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(m_inType, MF_MT_FRAME_RATE, &num, &den)) && den)
        MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, num, den);
    UINT32 parN = 1, parD = 1;
    if (SUCCEEDED(MFGetAttributeRatio(m_inType, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD)) && parD)
        MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, parN, parD);

    *type = mt;
    return S_OK;
}

static void ExtractAv1cObus(IMFMediaType *in, std::vector<unsigned char> &out) {
    UINT32 len = 0;
    if (FAILED(in->GetBlobSize(MF_MT_USER_DATA, &len)) || len <= 4) return;
    std::vector<unsigned char> blob(len);
    if (FAILED(in->GetBlob(MF_MT_USER_DATA, blob.data(), len, &len))) return;
    out.assign(blob.begin() + 4, blob.begin() + len);
}

STDMETHODIMP Av1DecMFT::SetInputType(DWORD id, IMFMediaType *type, DWORD flags) {
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (flags & MFT_SET_TYPE_TEST_ONLY) {
        if (!type) return E_INVALIDARG;
        GUID maj, sub;
        if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &maj)) || maj != MFMediaType_Video)
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != AV1_FMT)
            return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }

    SafeRelease(&m_inType);
    if (!type) { Reset(); return S_OK; }

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h)
        return MF_E_INVALIDMEDIATYPE;

    type->AddRef();
    m_inType = type;
    m_width = w;
    m_height = h;

    ExtractAv1cObus(type, m_obuCache);

    // no duration and the EVR has nothing to schedule
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) &&
        num && den) {
        m_frameDuration = (LONGLONG)(10000000.0 * den / num);
    }
    if (m_frameDuration <= 0) m_frameDuration = 166667;

    Av1MftLog("SetInputType: AV1 %ux%u, av1C configOBUs = %u bytes, frame duration = %lld",
              w, h, (unsigned)m_obuCache.size(), (long long)m_frameDuration);

    Reset();
    m_width = w;
    m_height = h;
    m_ready = true;
    return S_OK;
}

STDMETHODIMP Av1DecMFT::SetOutputType(DWORD id, IMFMediaType *type, DWORD flags) {
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (flags & MFT_SET_TYPE_TEST_ONLY) {
        if (!type) return E_INVALIDARG;
        GUID sub;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub))) return MF_E_INVALIDMEDIATYPE;
        if (sub != MFVideoFormat_NV12 && sub != MFVideoFormat_I420 &&
            sub != MFVideoFormat_YV12)
            return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }
    SafeRelease(&m_outType);
    if (!type) return S_OK;
    type->AddRef();
    m_outType = type;
    Av1MftLog("SetOutputType: NV12 accepted");
    return S_OK;
}

STDMETHODIMP Av1DecMFT::GetInputCurrentType(DWORD id, IMFMediaType **type) {
    if (!type) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!m_inType) return MF_E_TRANSFORM_TYPE_NOT_SET;
    m_inType->AddRef();
    *type = m_inType;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::GetOutputCurrentType(DWORD id, IMFMediaType **type) {
    if (!type) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!m_outType) return MF_E_TRANSFORM_TYPE_NOT_SET;
    m_outType->AddRef();
    *type = m_outType;
    return S_OK;
}

STDMETHODIMP Av1DecMFT::GetInputStatus(DWORD id, DWORD *flags) {
    if (!flags) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *flags = m_pending.empty() && m_frames.empty() ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::GetOutputStatus(DWORD *flags) {
    if (!flags) return E_POINTER;
    *flags = m_frames.empty() ? 0 : MFT_OUTPUT_STATUS_SAMPLE_READY;
    return S_OK;
}
STDMETHODIMP Av1DecMFT::SetOutputBounds(LONGLONG, LONGLONG) { return S_OK; }
STDMETHODIMP Av1DecMFT::ProcessEvent(DWORD, IMFMediaEvent *) { return E_NOTIMPL; }

STDMETHODIMP Av1DecMFT::ProcessMessage(MFT_MESSAGE_TYPE msg, ULONG_PTR) {
    if (msg == MFT_MESSAGE_COMMAND_FLUSH) {
        UINT32 w = m_width, h = m_height;
        Reset();
        m_width = w;
        m_height = h;
        m_ready = (m_inType != NULL);
    } else if (msg == MFT_MESSAGE_NOTIFY_END_OF_STREAM ||
               msg == MFT_MESSAGE_COMMAND_DRAIN) {
        // dav1d has no flush
    }
    return S_OK;
}

// EAGAIN means dav1d didn't take it, so the caller keeps the bytes and retries.
// data_create over data_wrap: wrap with a NULL free callback leaves the buffer ours.
static int SendCopy(Dav1dContext *ctx, const unsigned char *p, size_t n) {
    Dav1dData d;
    memset(&d, 0, sizeof(d));
    if (!dav1d_data_create(&d, n)) return -1;
    // data is declared const, but dav1d_data_create just gave us a writable buffer
    memcpy((void *)d.data, p, n);
    int r = dav1d_send_data(ctx, &d);
    if (r < 0) dav1d_data_unref(&d);   // not taken, so the reference is still ours
    return r;
}

bool Av1DecMFT::EnsureDecoder() {
    if (m_ctx) return true;
    if (dav1d_open(&m_ctx, &m_settings) != 0 || !m_ctx) {
        m_ctx = NULL;
        Av1MftLog("dav1d_open failed");
        return false;
    }
    Av1MftLog("dav1d decoder opened (%ux%u)", m_width, m_height);

    if (!m_obuFed && !m_obuCache.empty()) {
        int r = SendCopy(m_ctx, m_obuCache.data(), m_obuCache.size());
        if (r == 0) {
            m_obuFed = true;
            Av1MftLog("fed av1C sequence header (%u bytes)",
                      (unsigned)m_obuCache.size());
        } else {
            Av1MftLog("feeding av1C sequence header returned r=%d (ignored)", r);
        }
    }
    return true;
}

bool Av1DecMFT::PumpDecoder() {
    if (!EnsureDecoder()) return false;

    bool progress = false;
    for (;;) {
        if (!m_hasInFlight && !m_pending.empty()) {
            m_inFlight.swap(m_pending.front());
            m_inFlightTime = m_pendingTime.front();
            m_pending.pop_front();
            m_pendingTime.pop_front();
            m_hasInFlight = true;
        }

        if (m_hasInFlight) {
            int r = SendCopy(m_ctx, m_inFlight.data(), m_inFlight.size());
            if (r == 0) {
                m_inTimeQueue.push_back(m_inFlightTime);
                m_hasInFlight = false;
                m_inFlight.clear();
                progress = true;
            } else if (r == DAV1D_ERR(EAGAIN)) {
                // still full; draining a frame below frees a slot, then we retry
            } else {
                Av1MftLog("dav1d_send_data error r=%d, dropping this packet", r);
                m_hasInFlight = false;
                m_inFlight.clear();
                progress = true;
            }
        }

        Dav1dPicture pic;
        memset(&pic, 0, sizeof(pic));
        if (dav1d_get_picture(m_ctx, &pic) == 0) {
            m_frames.push_back(pic);
            progress = true;
            continue;
        }

        break;
    }
    return progress;
}

bool Av1DecMFT::ConvertToNV12(Dav1dPicture *pic, IMFSample *out) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(out->GetBufferByIndex(0, &buf))) return false;

    BYTE *dst = NULL;
    DWORD maxLen = 0;
    if (FAILED(buf->Lock(&dst, &maxLen, NULL))) { buf->Release(); return false; }

    const int w = pic->p.w, h = pic->p.h;
    const DWORD stride = GetFrameStride(w);
    bool ok = true;

    if (pic->p.layout != DAV1D_PIXEL_LAYOUT_I420) {
        Av1MftLog("unsupported pixel layout %d (only I420 is handled)", (int)pic->p.layout);
        ok = false;
    } else if (stride * (DWORD)h * 3 / 2 > maxLen) {
        Av1MftLog("output buffer too small: need %u, have %u", stride * (DWORD)h * 3 / 2, maxLen);
        ok = false;
    } else {
        const uint8_t *sy = (const uint8_t *)pic->data[0];
        const uint8_t *su = (const uint8_t *)pic->data[1];
        const uint8_t *sv = (const uint8_t *)pic->data[2];
        const ptrdiff_t lsy = pic->stride[0];
        const ptrdiff_t lsu = pic->stride[1];
        const ptrdiff_t lsv = pic->stride[1];

        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * stride, sy + (size_t)y * lsy, w);
        BYTE *uv = dst + (size_t)stride * h;
        for (int y = 0; y < h / 2; ++y) {
            BYTE *row = uv + (size_t)y * stride;
            const uint8_t *u = su + (size_t)y * lsu;
            const uint8_t *v = sv + (size_t)y * lsv;
            for (int x = 0; x < w / 2; ++x) {
                row[x * 2] = u[x];
                row[x * 2 + 1] = v[x];
            }
        }
        buf->SetCurrentLength(stride * (DWORD)h * 3 / 2);
    }

    buf->Unlock();
    buf->Release();
    return ok;
}

STDMETHODIMP Av1DecMFT::ProcessInput(DWORD id, IMFSample *sample, DWORD flags) {
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!sample) return E_INVALIDARG;
    if (!m_ready) return MF_E_TRANSFORM_TYPE_NOT_SET;

    IMFMediaBuffer *buf = NULL;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return E_FAIL;

    BYTE *p = NULL;
    DWORD len = 0;
    if (FAILED(buf->Lock(&p, NULL, &len))) { buf->Release(); return E_FAIL; }

    m_pending.push_back(std::vector<unsigned char>(p, p + len));
    LONGLONG t = 0;
    sample->GetSampleTime(&t);
    m_pendingTime.push_back(t);

    buf->Unlock();
    buf->Release();
    return S_OK;
}

STDMETHODIMP Av1DecMFT::ProcessOutput(DWORD, DWORD count,
                                      MFT_OUTPUT_DATA_BUFFER *buffers,
                                      DWORD *status) {
    if (!buffers || !status) return E_POINTER;
    if (count != 1) return E_INVALIDARG;
    *status = 0;

    if (!m_ready) return MF_E_TRANSFORM_TYPE_NOT_SET;

    while (m_frames.empty()) {
        if (!PumpDecoder()) break;
    }
    if (m_frames.empty()) return MF_E_TRANSFORM_NEED_MORE_INPUT;

    Dav1dPicture pic = m_frames.front();
    m_frames.pop_front();

    LONGLONG t = 0;
    if (!m_inTimeQueue.empty()) {
        t = m_inTimeQueue.front();
        m_inTimeQueue.pop_front();
    } else {
        t = m_lastTime + m_frameDuration;
    }
    m_lastTime = t;

    IMFSample *outSample = buffers[0].pSample;
    if (!outSample) {
        if (FAILED(MFCreateSample(&outSample))) {
            dav1d_picture_unref(&pic);
            return E_FAIL;
        }
        IMFMediaBuffer *mb = NULL;
        DWORD sz = GetFrameStride(pic.p.w) * pic.p.h * 3 / 2;
        if (FAILED(MFCreateMemoryBuffer(sz, &mb))) {
            outSample->Release();
            dav1d_picture_unref(&pic);
            return E_FAIL;
        }
        outSample->AddBuffer(mb);
        mb->Release();
        // MFCreateSample gives refcount 1; writing it here hands ownership to the
        // caller, and releasing it again kills the sample the renderer is about to read.
        buffers[0].pSample = outSample;
    }

    bool ok = ConvertToNV12(&pic, outSample);
    dav1d_picture_unref(&pic);
    if (!ok) {
        if (buffers[0].pSample) { buffers[0].pSample->Release(); buffers[0].pSample = NULL; }
        return E_FAIL;
    }

    outSample->SetSampleTime(t);
    outSample->SetSampleDuration(m_frameDuration);

    ++m_framesOut;
    if (m_framesOut <= 5 || (m_framesOut % 600) == 0)
        Av1MftLog("frame %ld output (%dx%d, t=%lld)", m_framesOut, m_width,
                  m_height, (long long)t);

    return S_OK;
}

class Av1ClassFactory : public IClassFactory {
  public:
    Av1ClassFactory() : m_ref(1) {}
    virtual ~Av1ClassFactory() {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
        if (ppv) *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        Av1DecMFT *mft = new (std::nothrow) Av1DecMFT();
        if (!mft) return E_OUTOFMEMORY;
        HRESULT hr = mft->QueryInterface(riid, ppv);
        mft->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

  private:
    LONG m_ref;
};

static bool g_av1Registered = false;

bool RegisterAv1Mft() {
    if (g_av1Registered) return true;

    Av1ClassFactory *factory = new (std::nothrow) Av1ClassFactory();
    if (!factory) return false;

    MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, AV1_FMT};
    MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_NV12};

    HRESULT hr = MFTRegisterLocal(factory, MFT_CATEGORY_VIDEO_DECODER,
                                  L"Artemis dav1d AV1 Decoder", 0, 1, &inInfo, 1,
                                  &outInfo);
    factory->Release();  // MFTRegisterLocal AddRef'd it internally

    if (FAILED(hr)) {
        Av1MftLog("MFTRegisterLocal failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    g_av1Registered = true;
    Av1MftLog("AV1 decoder MFT registered in-process (dav1d %s)",
              dav1d_version());
    return true;
}
