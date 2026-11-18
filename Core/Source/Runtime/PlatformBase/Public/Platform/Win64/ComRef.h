#pragma once

#include <Base/Compiler.h>

// Windows 전용이다. 다른 플랫폼에서 단독 include하면 아무 선언도 만들지 않는다(공개 헤더 단독
// 컴파일 검사가 모든 플랫폼에서 이 헤더를 컴파일한다).
#if ROCKS_PLATFORM_WINDOWS

#include <Windows.h>
#include <Unknwn.h>
#include <objbase.h>

// Windows COM 객체 참조를 소유하는 도구다(UE Core/Public/Microsoft/COMPointer.h의 TComPtr 대응).
// D3D11·D3D12·DXGI backend, DXC 호출, Windows 전용 도구가 함께 쓴다. Private/Win64 같은 Windows
// 전용 소스에서 포함한다.
//
// 사용 예:
//   ComRef<IDXGIFactory2> factory;
//   if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.Out())))) { return; }
//   ComRef<IDXGIFactory6> factory6;
//   if (SUCCEEDED(factory.TryQueryAs(factory6))) { factory6->EnumAdapterByGpuPreference(...); }
//   ComRef<ID3D12Resource> second = first.Share(); // AddRef한 두 번째 소유자
namespace rocks::platform::windows {

// COM 참조 하나를 소유한다. 소멸·Reset·Out이 Release한다.
// - 복사는 금지다. 참조를 하나 더 가져야 하면 Share()로 AddRef를 드러낸다(UE TComPtr의 복사
//   생성자 대응).
// - 이동은 참조 수를 바꾸지 않고 소유를 옮긴다. 옮긴 쪽은 비어 있다.
// - T는 IUnknown을 상속한 COM interface다. 스레드 안전성은 객체 자체의 AddRef·Release 규칙을
//   따르며, ComRef 하나를 여러 스레드가 동시에 바꾸면 안 된다.
template <class T>
class ComRef {
public:
    ComRef() noexcept = default;

    ~ComRef() noexcept {
        Reset();
    }

    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : pointer_(other.Detach()) {
    }

    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            Attach(other.Detach());
        }

        return *this;
    }

    // 쥔 참조를 놓고 비운다. 비어 있으면 아무것도 하지 않는다. Release가 소멸자를 부르다 이
    // ComRef를 다시 보더라도 이미 비어 있도록 먼저 비운 뒤 Release한다.
    void Reset() noexcept {
        T* pointer = pointer_;
        pointer_ = nullptr;
        if (pointer != nullptr) {
            pointer->Release();
        }
    }

    // 소유를 넘기지 않고 빌려준다. 비어 있으면 null이다. 반환값은 이 ComRef가 참조를 쥐는
    // 동안만 유효하다(API 인자로 넘길 때 쓴다).
    T* Get() const noexcept {
        return pointer_;
    }

    // 비어 있을 때 부르면 null 역참조다. 먼저 bool 변환이나 생성 함수의 HRESULT로 확인한다.
    T* operator->() const noexcept {
        return pointer_;
    }

    explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

    // COM 생성·조회 함수의 출력 인자로 넘길 주소다(UE TComPtr::GetInitReference). 기존 참조는
    // 먼저 놓으므로 같은 ComRef를 다시 채워도 새지 않는다. IID_PPV_ARGS(x.Out())처럼 쓴다.
    T** Out() noexcept {
        Reset();
        return &pointer_;
    }

    // 소유를 넘겨주고 비운다. 참조 수는 그대로이며 받은 쪽이 Release할 책임을 진다.
    [[nodiscard]] T* Detach() noexcept {
        T* pointer = pointer_;
        pointer_ = nullptr;
        return pointer;
    }

    // 이미 참조 하나를 가진 pointer의 소유를 넘겨받는다(AddRef하지 않는다). 기존 참조는 놓는다.
    // pointer가 null이면 비운 것과 같다. 다른 interface ComRef에서 옮길 때는
    // target.Attach(source.Detach())처럼 쓴다(T*로 암시 변환되는 형식만 받는다).
    void Attach(T* pointer) noexcept {
        T* previous = pointer_;
        pointer_ = pointer;
        if (previous != nullptr) {
            previous->Release();
        }
    }

    // 같은 객체를 가리키는 두 번째 소유자를 만든다(AddRef 한 번). 비어 있으면 빈 ComRef다.
    [[nodiscard]] ComRef Share() const noexcept {
        ComRef shared;
        if (pointer_ != nullptr) {
            pointer_->AddRef();
            shared.pointer_ = pointer_;
        }

        return shared;
    }

    // 같은 객체의 다른 interface U를 QueryInterface로 얻어 out에 담는다(UE
    // TComPtr::FromQueryInterface 대응). out의 기존 참조는 먼저 놓는다.
    // 반환: QueryInterface의 HRESULT 그대로다. 성공이면 out이 새 참조를 쥔다. 지원하지 않으면
    // E_NOINTERFACE이고 out은 비어 있다. 이 ComRef가 비어 있으면 E_POINTER다.
    template <class U>
    HRESULT TryQueryAs(ComRef<U>& out) const noexcept {
        out.Reset();
        if (pointer_ == nullptr) {
            return E_POINTER;
        }

        return pointer_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.Out()));
    }

private:
    T* pointer_ = nullptr;
};

} // namespace rocks::platform::windows

#endif
