#include "Win64TextServices.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Core/Move.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <olectl.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

namespace {

inline constexpr LogCategory LogWin64TextServices{"Platform.TextServices"};
// 저장소의 보기는 하나다(UE FTextStoreACP::GetActiveView — 0).
constexpr TsViewCookie TextStoreView = 0;

// 잠금 종류 판정이다(ITextStoreACP — TS_LF_READ는 읽기, TS_LF_READWRITE는 읽기·쓰기).
bool CanRead(DWORD lock) noexcept {
    return (lock & TS_LF_READ) == TS_LF_READ;
}

bool CanWrite(DWORD lock) noexcept {
    return (lock & TS_LF_READWRITE) == TS_LF_READWRITE;
}

// allocator로 COM 객체를 만든다(참조 1). Release가 0이 되면 스스로 DestroyAt·Deallocate한다.
template <typename T, typename... Args>
T* MakeComObject(Allocator& allocator, Args&&... args) noexcept {
    auto storage = allocator.Allocate(sizeof(T), alignof(T));
    return storage ? InitializeAt<T>(storage.Value(), allocator, Forward<Args>(args)...) : nullptr;
}

template <typename T>
ULONG ReleaseComObject(T* object, Allocator& allocator, ULONG& references) noexcept {
    const ULONG left = --references;
    if (left == 0) {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            DestroyAt(object);
        }

        (void)allocator.Deallocate(object);
    }

    return left;
}

// client 사각형을 화면 사각형으로 바꾼다(TSF 위치는 화면 좌표다).
bool ClientToScreenRect(HWND window, const Box2<float>& box, RECT& out) noexcept {
    int32 left = 0, top = 0, right = 0, bottom = 0;
    if (!math::TryRoundToInt32(box.min.x, left) || !math::TryRoundToInt32(box.min.y, top) ||
        !math::TryRoundToInt32(box.max.x, right) || !math::TryRoundToInt32(box.max.y, bottom)) {
        return false;
    }

    POINT first{left, top};
    POINT second{right, bottom};
    if (!ClientToScreen(window, &first) || !ClientToScreen(window, &second)) {
        return false;
    }

    out = {first.x, first.y, second.x, second.y};
    return true;
}

// ITfRange의 ACP 범위다(UE FTextStoreACP::OnUpdateComposition).
bool RangeExtent(ITfRange* range, uint32& start, uint32& end) noexcept {
    if (range == nullptr) {
        return false;
    }

    windows::ComRef<ITfRangeACP> acp;
    LONG first = 0;
    LONG length = 0;
    if (FAILED(range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(acp.Out()))) ||
        FAILED(acp->GetExtent(&first, &length)) || first < 0 || length < 0) {
        return false;
    }

    start = static_cast<uint32>(first);
    end = static_cast<uint32>(first + length);
    return true;
}

} // namespace

// TSF 문서 저장소다(ITextStoreACP·ITfContextOwnerCompositionSink — UE FTextStoreACP). 문서 내용은
// 거울(TextInputDocument)이고, 위치 질의만 문맥에 바로 묻는다(읽기만 — 22.13절). 바깥 잠금을 풀 때
// 서비스가 거울의 바뀐 것을 기록으로 낸다. 문서를 떼면 Unbind로 문맥·서비스를 끊는다(TSF가 참조를
// 더 쥐고 있어도 그 뒤 호출은 실패로 답한다).
class Win64TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
public:
    Win64TextStore(Allocator& allocator, HWND window) noexcept
        : allocator_(allocator), window_(window) {
    }

    ~Win64TextStore() noexcept = default;

    void Bind(Win64TextServices& services, TextInputClient& client) noexcept {
        services_ = &services;
        client_ = &client;
    }

    void Unbind() noexcept {
        services_ = nullptr;
        client_ = nullptr;
        composition_.Reset();
        sink_.Reset();
        sinkMask_ = 0;
    }

    TextInputDocument& Document() noexcept {
        return document_;
    }

    bool IsLocked() const noexcept {
        return lock_ != 0;
    }

    bool IsComposing() const noexcept {
        return static_cast<bool>(composition_);
    }

    ITextStoreACPSink* Sink(DWORD mask) const noexcept {
        return sink_ && (sinkMask_ & mask) == mask ? sink_.Get() : nullptr;
    }

    // IUnknown.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ITextStoreACP)) {
            *object = static_cast<ITextStoreACP*>(this);
        } else if (IsEqualIID(id, IID_ITfContextOwnerCompositionSink)) {
            *object = static_cast<ITfContextOwnerCompositionSink*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseComObject(this, allocator_, references_);
    }

    // ITextStoreACP — 알림 sink.
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown* unknown, DWORD mask) override {
        if (!IsEqualIID(id, IID_ITextStoreACPSink) || unknown == nullptr) {
            return E_INVALIDARG;
        }

        windows::ComRef<IUnknown> identity;
        if (FAILED(
                unknown->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(identity.Out())))) {
            return E_INVALIDARG;
        }

        // 같은 sink를 다시 알리면 mask만 바꾼다. 다른 sink는 하나만 받는다(ITextStoreACP 규약).
        if (sink_) {
            windows::ComRef<IUnknown> current;
            if (SUCCEEDED(
                    sink_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(current.Out()))) &&
                current.Get() == identity.Get()) {
                sinkMask_ = mask;
                return S_OK;
            }

            return CONNECT_E_ADVISELIMIT;
        }

        if (FAILED(unknown->QueryInterface(IID_ITextStoreACPSink,
                                           reinterpret_cast<void**>(sink_.Out())))) {
            return E_UNEXPECTED;
        }

        sinkMask_ = mask;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* unknown) override {
        if (unknown == nullptr || !sink_) {
            return CONNECT_E_NOCONNECTION;
        }

        sink_.Reset();
        sinkMask_ = 0;
        return S_OK;
    }

    // 잠금(UE FTextStoreACP::RequestLock 177–235행): 잠겨 있지 않으면 바로 허락하고, 읽기 잠금 중의
    // 비동기 쓰기 청은 허락 뒤 한 번 더 준다. 잠긴 채 동기 청은 TS_E_SYNCHRONOUS다. 바깥 잠금을 풀
    // 때 거울의 바뀐 것을 기록으로 낸다.
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT* session) override {
        if (!sink_ || session == nullptr) {
            return session == nullptr ? E_INVALIDARG : E_FAIL;
        }

        if (lock_ == 0) {
            lock_ = flags & ~static_cast<DWORD>(TS_LF_SYNC);
            *session = sink_->OnLockGranted(lock_);
            while (pendingUpgrade_ && sink_) {
                pendingUpgrade_ = false;
                lock_ = TS_LF_READWRITE;
                (void)sink_->OnLockGranted(lock_);
            }

            lock_ = 0;
            pendingUpgrade_ = false;
            if (services_) {
                services_->FlushEdits();
            }

            return S_OK;
        }

        if (!CanWrite(lock_) && CanWrite(flags) && (flags & TS_LF_SYNC) == 0) {
            pendingUpgrade_ = true;
            *session = TS_S_ASYNC;
            return S_OK;
        }

        *session = TS_E_SYNCHRONOUS;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
        if (status == nullptr) {
            return E_INVALIDARG;
        }

        // 읽기 전용 문맥에는 문서를 만들지 않는다(IME 끔 — 22.13절).
        status->dwDynamicFlags = 0;
        status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInsert(LONG start, LONG end, ULONG, LONG* resultStart,
                                          LONG* resultEnd) override {
        const LONG length = static_cast<LONG>(document_.Length());
        if (resultStart == nullptr || resultEnd == nullptr) {
            return E_INVALIDARG;
        }

        if (start < 0 || start > end || end > length) {
            return E_INVALIDARG;
        }

        *resultStart = start;
        *resultEnd = end;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection,
                                           ULONG* fetched) override {
        if (!CanRead(lock_)) {
            return TS_E_NOLOCK;
        }

        if (selection == nullptr || fetched == nullptr) {
            return E_INVALIDARG;
        }

        // 선택은 하나다(UE와 같다).
        *fetched = 0;
        if (index != TS_DEFAULT_SELECTION && index != 0) {
            return TS_E_NOSELECTION;
        }

        if (count == 0) {
            return S_OK;
        }

        selection[0].acpStart = static_cast<LONG>(document_.SelectionStart());
        selection[0].acpEnd = static_cast<LONG>(document_.SelectionEnd());
        selection[0].style.ase = document_.CaretAtStart() ? TS_AE_START : TS_AE_END;
        selection[0].style.fInterimChar = FALSE;
        *fetched = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSelection(ULONG count,
                                           const TS_SELECTION_ACP* selection) override {
        if (!CanWrite(lock_)) {
            return TS_E_NOLOCK;
        }

        if (count == 0 || selection == nullptr) {
            return E_INVALIDARG;
        }

        if (selection[0].acpStart < 0 || selection[0].acpEnd < selection[0].acpStart ||
            !document_.SetSelection(static_cast<uint32>(selection[0].acpStart),
                                    static_cast<uint32>(selection[0].acpEnd),
                                    selection[0].style.ase == TS_AE_START)) {
            return TF_E_INVALIDPOS;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetText(LONG start, LONG end, WCHAR* plain, ULONG plainCapacity,
                                      ULONG* plainCount, TS_RUNINFO* runs, ULONG runCapacity,
                                      ULONG* runCount, LONG* next) override {
        if (!CanRead(lock_)) {
            return TS_E_NOLOCK;
        }

        if (plainCount == nullptr || runCount == nullptr || next == nullptr) {
            return E_INVALIDARG;
        }

        const LONG length = static_cast<LONG>(document_.Length());
        if (end == -1) {
            end = length;
        }

        if (start < 0 || start > length || end < start || end > length) {
            return TF_E_INVALIDPOS;
        }

        // 평문 버퍼만큼 내준다. 평문을 청하지 않으면 run만으로 나아간다(UE 같은 처리).
        const Span<const char16_t> text = document_.Text();
        const ULONG range = static_cast<ULONG>(end - start);
        const bool wantsPlain = plain != nullptr && plainCapacity > 0;
        const ULONG copied = wantsPlain ? (range < plainCapacity ? range : plainCapacity) : 0;
        for (ULONG i = 0; i < copied; ++i) {
            plain[i] = static_cast<WCHAR>(text[static_cast<usize>(start) + i]);
        }

        *plainCount = copied;
        const ULONG advanced = wantsPlain ? copied : range;
        *runCount = 0;
        if (runs != nullptr && runCapacity > 0 && advanced > 0) {
            runs[0].uCount = advanced;
            runs[0].type = TS_RT_PLAIN;
            *runCount = 1;
        }

        *next = start + static_cast<LONG>(advanced);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG count,
                                      TS_TEXTCHANGE* change) override {
        if (!CanWrite(lock_)) {
            return TS_E_NOLOCK;
        }

        if ((count != 0 && text == nullptr) || change == nullptr) {
            return E_INVALIDARG;
        }

        TextInputChange changed;
        if (start < 0 || end < start ||
            !document_.Replace(static_cast<uint32>(start), static_cast<uint32>(end),
                               {reinterpret_cast<const char16_t*>(text), count}, changed)) {
            return TF_E_INVALIDPOS;
        }

        *change = {static_cast<LONG>(changed.start), static_cast<LONG>(changed.oldEnd),
                   static_cast<LONG>(changed.newEnd)};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count,
                                                    LONG* start, LONG* end,
                                                    TS_TEXTCHANGE* change) override {
        if ((flags & TS_IAS_QUERYONLY) != 0) {
            if (!CanRead(lock_)) {
                return TS_E_NOLOCK;
            }

            if (start == nullptr || end == nullptr) {
                return E_INVALIDARG;
            }

            *start = static_cast<LONG>(document_.SelectionStart());
            *end = static_cast<LONG>(document_.SelectionEnd());
            return S_OK;
        }

        if (!CanWrite(lock_)) {
            return TS_E_NOLOCK;
        }

        if ((count != 0 && text == nullptr) || change == nullptr ||
            ((flags & TS_IAS_NOQUERY) == 0 && (start == nullptr || end == nullptr))) {
            return E_INVALIDARG;
        }

        TextInputChange changed;
        if (!document_.InsertAtSelection({reinterpret_cast<const char16_t*>(text), count},
                                         changed)) {
            return E_FAIL;
        }

        *change = {static_cast<LONG>(changed.start), static_cast<LONG>(changed.oldEnd),
                   static_cast<LONG>(changed.newEnd)};
        if ((flags & TS_IAS_NOQUERY) == 0) {
            *start = static_cast<LONG>(changed.start);
            *end = static_cast<LONG>(changed.newEnd);
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override {
        if (!CanRead(lock_)) {
            return TS_E_NOLOCK;
        }

        if (end == nullptr) {
            return E_INVALIDARG;
        }

        *end = static_cast<LONG>(document_.Length());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override {
        if (view == nullptr) {
            return E_INVALIDARG;
        }

        *view = TextStoreView;
        return S_OK;
    }

    // 위치 질의는 문맥에 바로 묻는다(읽기만). 거울이 문맥보다 앞서 있으면(적용 전 기록) 조합
    // 시작(없으면 caret) 자리로 근사한다(22.13절).
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rect,
                                         BOOL* clipped) override {
        if (!CanRead(lock_)) {
            return TS_E_NOLOCK;
        }

        if (view != TextStoreView || rect == nullptr || clipped == nullptr) {
            return E_INVALIDARG;
        }

        const LONG length = static_cast<LONG>(document_.Length());
        if (end == -1) {
            end = length;
        }

        if (start < 0 || start > length || end < start || end > length) {
            return TS_E_INVALIDPOS;
        }

        if (client_ == nullptr || services_ == nullptr) {
            return TS_E_NOLAYOUT;
        }

        uint32 begin = static_cast<uint32>(start);
        uint32 span = static_cast<uint32>(end - start);
        if (document_.HasUnflushedEdits() || services_->HasPendingRecords()) {
            begin =
                document_.IsComposing() ? document_.CompositionStart() : document_.SelectionStart();
            span = 0;
        }

        Box2<float> box;
        if (!client_->GetTextBounds(begin, span, box) || !ClientToScreenRect(window_, box, *rect)) {
            return TS_E_NOLAYOUT;
        }

        *clipped = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view, RECT* rect) override {
        if (view != TextStoreView || rect == nullptr) {
            return E_INVALIDARG;
        }

        Box2<float> box;
        if (client_ == nullptr || !client_->GetScreenBounds(box) ||
            !ClientToScreenRect(window_, box, *rect)) {
            *rect = {};
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD,
                                              LONG* position) override {
        if (!CanRead(lock_)) {
            return TS_E_NOLOCK;
        }

        if (view != TextStoreView || point == nullptr || position == nullptr) {
            return E_INVALIDARG;
        }

        POINT local = *point;
        if (client_ == nullptr || !ScreenToClient(window_, &local)) {
            return TS_E_INVALIDPOINT;
        }

        const Optional<uint32> index = client_->GetCharacterIndexFromPoint(
            {static_cast<float>(local.x), static_cast<float>(local.y)});
        if (!index) {
            return TS_E_INVALIDPOINT;
        }

        *position = static_cast<LONG>(*index < document_.Length() ? *index : document_.Length());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND* window) override {
        if (view != TextStoreView || window == nullptr) {
            return E_INVALIDARG;
        }

        *window = window_;
        return S_OK;
    }

    // 속성은 내지 않는다(빈 결과 — 22.13절). embedded는 다루지 않는다(UE와 같다).
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*,
                                                     DWORD) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*,
                                                                  DWORD) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG halt, ULONG, const TS_ATTRID*,
                                                     DWORD, LONG* next, BOOL* found,
                                                     LONG* offset) override {
        if (next == nullptr || found == nullptr || offset == nullptr) {
            return E_INVALIDARG;
        }

        *next = halt;
        *found = FALSE;
        *offset = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) override {
        if (fetched == nullptr) {
            return E_INVALIDARG;
        }

        *fetched = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown** unknown) override {
        if (unknown != nullptr) {
            *unknown = nullptr;
        }

        return CanRead(lock_) ? E_NOTIMPL : TS_E_NOLOCK;
    }

    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                                  BOOL* insertable) override {
        if (insertable != nullptr) {
            *insertable = FALSE;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject** data) override {
        if (data != nullptr) {
            *data = nullptr;
        }

        return CanRead(lock_) ? E_NOTIMPL : TS_E_NOLOCK;
    }

    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*,
                                             TS_TEXTCHANGE*) override {
        return CanWrite(lock_) ? E_NOTIMPL : TS_E_NOLOCK;
    }

    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                                        TS_TEXTCHANGE*) override {
        return CanWrite(lock_) ? E_NOTIMPL : TS_E_NOLOCK;
    }

    // ITfContextOwnerCompositionSink(UE FTextStoreACP 764–850행). 시작·끝은 잠금 안이라도 그
    // 자리에서 기록을 낸다 — 한 잠금 안의 끝 → 시작이 확정·시작으로 나뉘어야 앞 음절이 확정된다
    // (22.13절 조합 경계 — 큐 기록이라 TSF 알림 금지와 무관). 범위 갱신은 잠금을 풀 때 낸다.
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView* view,
                                                 BOOL* accepted) override {
        if (accepted == nullptr) {
            return E_INVALIDARG;
        }

        if (composition_ || view == nullptr) {
            *accepted = FALSE;
            return S_OK;
        }

        // 시작 전의 편집(조합 밖 삽입 등)은 따로 확정 편집으로 낸다.
        FlushNow();
        view->AddRef();
        composition_.Attach(view);
        windows::ComRef<ITfRange> range;
        uint32 start = document_.SelectionStart();
        uint32 end = document_.SelectionEnd();
        if (SUCCEEDED(view->GetRange(range.Out()))) {
            (void)RangeExtent(range.Get(), start, end);
        }

        document_.StartComposition(start, end);
        *accepted = TRUE;
        FlushOutsideLock();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView* view,
                                                  ITfRange* range) override {
        if (!composition_ || view != composition_.Get()) {
            return E_UNEXPECTED;
        }

        uint32 start = 0;
        uint32 end = 0;
        if (RangeExtent(range, start, end)) {
            document_.UpdateComposition(start, end);
        }

        FlushOutsideLock();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView* view) override {
        if (!composition_ || view != composition_.Get()) {
            return E_UNEXPECTED;
        }

        composition_.Reset();
        document_.EndComposition();
        FlushNow();
        return S_OK;
    }

private:
    void FlushOutsideLock() noexcept {
        if (lock_ == 0) {
            FlushNow();
        }
    }

    void FlushNow() noexcept {
        if (services_) {
            services_->FlushEdits();
        }
    }

    Allocator& allocator_;
    HWND window_;
    ULONG references_ = 1;
    Win64TextServices* services_ = nullptr;
    TextInputClient* client_ = nullptr;
    TextInputDocument document_;
    windows::ComRef<ITextStoreACPSink> sink_;
    DWORD sinkMask_ = 0;
    DWORD lock_ = 0;
    bool pendingUpgrade_ = false;
    windows::ComRef<ITfCompositionView> composition_;
};

// 활성 입력기 바뀜을 받는다(ITfInputProcessorProfileActivationSink — UE FTSFActivationProxy).
class Win64ProfileSink final : public ITfInputProcessorProfileActivationSink {
public:
    Win64ProfileSink(Allocator& allocator, Win64TextServices& services) noexcept
        : allocator_(allocator), services_(&services) {
    }

    ~Win64ProfileSink() noexcept = default;

    void Unbind() noexcept {
        services_ = nullptr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (IsEqualIID(id, IID_IUnknown) ||
            IsEqualIID(id, IID_ITfInputProcessorProfileActivationSink)) {
            *object = static_cast<ITfInputProcessorProfileActivationSink*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseComObject(this, allocator_, references_);
    }

    HRESULT STDMETHODCALLTYPE OnActivated(DWORD, LANGID, REFCLSID, REFGUID, REFGUID, HKL,
                                          DWORD flags) override {
        if (services_ && (flags & TF_IPSINK_FLAG_ACTIVE) != 0) {
            services_->OnProfileActivated();
        }

        return S_OK;
    }

private:
    Allocator& allocator_;
    Win64TextServices* services_;
    ULONG references_ = 1;
};

Win64TextServices::Win64TextServices(Allocator& allocator, TextInputSession& session) noexcept
    : allocator_(allocator), session_(session) {
}

Win64TextServices::~Win64TextServices() noexcept {
    Shutdown();
}

bool Win64TextServices::Initialize(HWND window, MarkerSink&& markers,
                                   Delegate<void()>&& profileChanged) noexcept {
    window_ = window;
    markers_ = Move(markers);
    profileChanged_ = Move(profileChanged);
    // TSF는 STA COM이 필요하다(UE FWindowsPlatformMisc::CoInitialize). 이미 다른 모드면 IMM만 쓴다.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        Log::Info(LogWin64TextServices,
                  "COM is not single-threaded on the window thread; IME uses IMM only");
        return false;
    }

    comInitialized_ = true;
    if (FAILED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                reinterpret_cast<void**>(threadManager_.Out()))) ||
        FAILED(threadManager_->Activate(&clientId_))) {
        Log::Info(LogWin64TextServices, "Text Services Framework is unavailable; IME uses IMM");
        threadManager_.Reset();
        return false;
    }

    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfileMgr,
                                reinterpret_cast<void**>(profiles_.Out()))) ||
        FAILED(threadManager_->QueryInterface(IID_ITfSource,
                                              reinterpret_cast<void**>(source_.Out())))) {
        Shutdown();
        return false;
    }

    profileSink_ = MakeComObject<Win64ProfileSink>(allocator_, *this);
    if (profileSink_ == nullptr ||
        FAILED(source_->AdviseSink(
            IID_ITfInputProcessorProfileActivationSink,
            static_cast<ITfInputProcessorProfileActivationSink*>(profileSink_), &profileCookie_))) {
        profileCookie_ = TF_INVALID_COOKIE;
        Shutdown();
        return false;
    }

    available_ = true;
    RefreshProfile();
    return true;
}

void Win64TextServices::Shutdown() noexcept {
    Detach(false);
    if (source_ && profileCookie_ != TF_INVALID_COOKIE) {
        (void)source_->UnadviseSink(profileCookie_);
    }

    profileCookie_ = TF_INVALID_COOKIE;
    if (profileSink_) {
        profileSink_->Unbind();
        profileSink_->Release();
        profileSink_ = nullptr;
    }

    source_.Reset();
    profiles_.Reset();
    if (threadManager_) {
        (void)threadManager_->Deactivate();
        threadManager_.Reset();
    }

    clientId_ = TF_CLIENTID_NULL;
    available_ = false;
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

bool Win64TextServices::IsInputProcessorActive() const noexcept {
    return available_ && inputProcessor_;
}

bool Win64TextServices::HasDocument() const noexcept {
    return store_ != nullptr;
}

void Win64TextServices::RefreshProfile() noexcept {
    // 활성 키보드 입력기가 TIP이면 TSF, 키보드 배치(옛 IMM 입력기)면 IMM이다(UE 327·351행).
    TF_INPUTPROCESSORPROFILE profile{};
    inputProcessor_ = profiles_ &&
                      SUCCEEDED(profiles_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)) &&
                      profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR;
}

void Win64TextServices::OnProfileActivated() noexcept {
    const bool before = inputProcessor_;
    RefreshProfile();
    if (before != inputProcessor_) {
        profileChanged_.ExecuteIfBound();
    }
}

bool Win64TextServices::Attach(TextInputClient& client) noexcept {
    Detach(false);
    if (!IsInputProcessorActive() || !client.AllowsInputMethod()) {
        return false;
    }

    const uint32 length = client.GetTextLength();
    if (length > MaxTextInputDocumentCodeUnits) {
        if (!loggedTooLong_) {
            loggedTooLong_ = true;
            Log::Warning(LogWin64TextServices,
                         "Text of {Length} code units exceeds the TSF document capacity; IME uses "
                         "IMM for it",
                         length);
        }

        return false;
    }

    Array<char16_t> text(allocator_);
    if (!client.GetText(0, length, text)) {
        return false;
    }

    Win64TextStore* store = MakeComObject<Win64TextStore>(allocator_, window_);
    if (store == nullptr) {
        return false;
    }

    uint32 begin = 0;
    uint32 selected = 0;
    client.GetSelection(begin, selected);
    (void)store->Document().Sync(text.View(), begin, selected);
    store->Bind(*this, client);
    TfEditCookie cookie = 0;
    windows::ComRef<ITfDocumentMgr> document;
    windows::ComRef<ITfContext> context;
    const bool created =
        SUCCEEDED(threadManager_->CreateDocumentMgr(document.Out())) &&
        SUCCEEDED(document->CreateContext(clientId_, 0, static_cast<ITextStoreACP*>(store),
                                          context.Out(), &cookie)) &&
        SUCCEEDED(document->Push(context.Get()));
    if (!created) {
        store->Unbind();
        store->Release();
        return false;
    }

    store_ = store;
    client_ = &client;
    document_ = Move(document);
    context_ = Move(context);
    (void)context_->QueryInterface(IID_ITfContextOwnerCompositionServices,
                                   reinterpret_cast<void**>(compositionServices_.Out()));
    // 창이 초점을 받으면 TSF가 이 문서로 옮긴다. 이미 초점이 있으면 바로 옮긴다.
    ITfDocumentMgr* previous = nullptr;
    if (SUCCEEDED(threadManager_->AssociateFocus(window_, document_.Get(), &previous))) {
        previous_.Attach(previous);
    }

    if (GetFocus() == window_) {
        (void)threadManager_->SetFocus(document_.Get());
    }

    return true;
}

void Win64TextServices::Detach(bool complete) noexcept {
    if (store_ == nullptr) {
        return;
    }

    if (complete) {
        TerminateComposition();
    }

    // 창의 문서 연결을 붙이기 전으로 되돌린다. 되돌린 문서 관리자가 있고 창에 초점이 있으면 거기에
    // 초점을 둔다. 없으면 창이 WM_IME_SETCONTEXT를 다시 보내(RefreshInputMethodContext) CUAS가 IMM
    // 문맥 문서로 초점을 옮긴다 — 문맥 밖 게임 문자 입력은 IMM 경로다(22.13절).
    ITfDocumentMgr* replaced = nullptr;
    if (SUCCEEDED(threadManager_->AssociateFocus(window_, previous_.Get(), &replaced)) &&
        replaced != nullptr) {
        replaced->Release();
    }

    if (GetFocus() == window_ && previous_) {
        (void)threadManager_->SetFocus(previous_.Get());
    }

    previous_.Reset();
    compositionServices_.Reset();
    if (document_) {
        (void)document_->Pop(TF_POPF_ALL);
    }

    context_.Reset();
    document_.Reset();
    Win64TextStore* store = store_;
    store_ = nullptr;
    client_ = nullptr;
    store->Unbind();
    store->Release();
}

void Win64TextServices::TerminateComposition() noexcept {
    if (store_ && store_->IsComposing() && compositionServices_) {
        // 조합을 확정한다(IMM CPS_COMPLETE와 같은 결과 — UE CancelComposition 187행).
        (void)compositionServices_->TerminateComposition(nullptr);
    }
}

void Win64TextServices::Sync(bool layoutChanged) noexcept {
    if (store_ == nullptr || client_ == nullptr || store_->IsLocked()) {
        return;
    }

    TextInputDocument& document = store_->Document();
    if (session_.PendingCount() == 0 && !document.HasUnflushedEdits()) {
        const uint32 length = client_->GetTextLength();
        if (length > MaxTextInputDocumentCodeUnits) {
            // 멈춘 거울로 엉뚱한 자리에 쓰지 않게 문서를 떼고 IMM으로 받는다(22.13절).
            if (!loggedTooLong_) {
                loggedTooLong_ = true;
                Log::Warning(LogWin64TextServices,
                             "Text grew to {Length} code units, over the TSF document capacity; "
                             "IME uses IMM for it",
                             length);
            }

            Detach(true);
            return;
        }

        Array<char16_t> text(allocator_);
        if (client_->GetText(0, length, text)) {
            uint32 begin = 0;
            uint32 selected = 0;
            client_->GetSelection(begin, selected);
            const TextInputSyncResult synced = document.Sync(text.View(), begin, selected);
            if (ITextStoreACPSink* sink =
                    synced.textChanged ? store_->Sink(TS_AS_TEXT_CHANGE) : nullptr) {
                TS_TEXTCHANGE change{static_cast<LONG>(synced.change.start),
                                     static_cast<LONG>(synced.change.oldEnd),
                                     static_cast<LONG>(synced.change.newEnd)};
                (void)sink->OnTextChange(0, &change);
            }

            if (ITextStoreACPSink* sink =
                    synced.selectionChanged ? store_->Sink(TS_AS_SEL_CHANGE) : nullptr) {
                (void)sink->OnSelectionChange();
            }
        }
    }

    if (ITextStoreACPSink* sink = layoutChanged ? store_->Sink(TS_AS_LAYOUT_CHANGE) : nullptr) {
        (void)sink->OnLayoutChange(TS_LC_CHANGE, TextStoreView);
    }
}

bool Win64TextServices::HasPendingRecords() const noexcept {
    return session_.PendingCount() != 0;
}

void Win64TextServices::FlushEdits() noexcept {
    if (store_ == nullptr) {
        return;
    }

    const TextInputRecordResult result = store_->Document().Flush(session_);
    if (result.serial || result.overflowed) {
        markers_.ExecuteIfBound(result);
    }
}

} // namespace rocks::platform
