#pragma once

#include "../Common/TextInputDocument.h"

#include <Core/Delegate.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Win64/ComRef.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <msctf.h>
#include <textstor.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 창 하나의 TSF 연결이다(Docs/Design/UiToolkit 22.13절 — UE FWindowsTextInputMethodSystem의 TSF
// 경로). 활성 입력기가 TSF 입력기(TIP)이고 문맥이 IME를 쓰면 문맥마다 TSF 문서(거울 문서를 가진
// 저장소)를 만들어 창에 붙인다. 아니면 창은 지금의 IMM 경로를 쓴다. TSF가 고친 거울은 잠금을 풀 때
// 창 TextInputSession 기록이 되고, 창이 그 표지를 입력 큐에 넣는다.
//
// 스레드: 창 소유 스레드(게임 lane)에서만 쓴다. TSF 콜백도 그 스레드의 메시지 펌프 안에서 온다.
//
// 사용 예(Win64Window):
//   services.Initialize(window, markers, profileChanged);   // 창을 만든 뒤 — 실패하면 IMM만
//   if (services.Attach(client)) { /* IMM 메시지는 기록하지 않는다 */ }
//   services.Detach(true);                                  // 조합 확정 → 기록 → 문서 뗌
namespace rocks::platform {

class Win64TextStore;
class Win64ProfileSink;

class Win64TextServices final {
public:
    using MarkerSink = Delegate<void(const TextInputRecordResult&)>;

    Win64TextServices(Allocator& allocator, TextInputSession& session) noexcept;
    ~Win64TextServices() noexcept;

    Win64TextServices(const Win64TextServices&) = delete;
    Win64TextServices& operator=(const Win64TextServices&) = delete;

    // COM(STA)·스레드 관리자·입력기 프로필 sink를 연다. markers는 잠금을 풀 때 낸 기록 결과를,
    // profileChanged는 활성 입력기 종류(TIP·IMM)가 바뀐 것을 받는다(OS 콜백 안이므로 받는 쪽은
    // 표시만 하고 나중에 처리한다). 실패하면 false고 창은 IMM만 쓴다.
    bool Initialize(HWND window, MarkerSink&& markers, Delegate<void()>&& profileChanged) noexcept;
    void Shutdown() noexcept;

    // 활성 입력기가 TSF 입력기(TIP)인지다.
    bool IsInputProcessorActive() const noexcept;
    // 지금 TSF 문서로 받는 문맥이 있는지다(창은 IMM 메시지를 기록하지 않는다).
    bool HasDocument() const noexcept;

    // 문맥을 TSF 문서로 붙인다. TSF가 없거나 TIP가 아니거나 IME를 쓰지 않는 문맥이거나 글자가 거울
    // 용량을 넘으면 붙이지 않고 false다(IMM 경로).
    bool Attach(TextInputClient& client) noexcept;
    // complete면 조합을 확정하고(TerminateComposition — 그 잠금이 기록을 낸다) 문서를 뗀다.
    // 호출자가 그다음 session.Deactivate로 남은 기록을 처리한다.
    void Detach(bool complete) noexcept;
    // TSF 조합을 끝낸다(앱이 조합 중에 글자를 바꿈·기록 넘침). 그 잠금이 낸 기록은 호출자가
    // 버리거나 처리한 뒤 Sync로 거울을 다시 맞춘다.
    void TerminateComposition() noexcept;
    // 적용 전 기록이 없으면 문맥 글자·선택으로 거울을 맞추고 TSF에 알린다(OnTextChange·
    // OnSelectionChange). layoutChanged면 OnLayoutChange도 보낸다.
    void Sync(bool layoutChanged) noexcept;

    // 프로필 sink가 부른다(입력기 바뀜).
    void OnProfileActivated() noexcept;
    // 문맥에 아직 적용하지 않은 기록이 있는지다(저장소의 위치 근사 판정).
    bool HasPendingRecords() const noexcept;
    // 저장소가 바깥 잠금을 풀 때 부른다 — 거울의 바뀐 것을 기록으로 낸다.
    void OnUnlocked() noexcept;

private:
    void RefreshProfile() noexcept;

    Allocator& allocator_;
    TextInputSession& session_;
    HWND window_ = nullptr;
    MarkerSink markers_;
    Delegate<void()> profileChanged_;
    bool comInitialized_ = false;
    bool available_ = false;
    bool inputProcessor_ = false;
    bool loggedTooLong_ = false;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    windows::ComRef<ITfThreadMgr> threadManager_;
    windows::ComRef<ITfInputProcessorProfileMgr> profiles_;
    windows::ComRef<ITfSource> source_;
    Win64ProfileSink* profileSink_ = nullptr;
    DWORD profileCookie_ = TF_INVALID_COOKIE;
    // 붙은 문서다.
    Win64TextStore* store_ = nullptr;
    windows::ComRef<ITfDocumentMgr> document_;
    windows::ComRef<ITfContext> context_;
    windows::ComRef<ITfContextOwnerCompositionServices> compositionServices_;
    // 붙이기 전 창에 이어져 있던 문서 관리자다(떼면 되돌린다 — 없으면 null, CUAS·IMM 기본).
    windows::ComRef<ITfDocumentMgr> previous_;
    TextInputClient* client_ = nullptr;
};

} // namespace rocks::platform
