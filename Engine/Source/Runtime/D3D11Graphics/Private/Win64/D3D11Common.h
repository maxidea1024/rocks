#pragma once

#include <Base/NumericLimits.h>
#include <Graphics/DeviceTypes.h>

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

// D3D11Graphics 내부 공용 도구다(COM 참조·형식 표·진단 문자열). 장치 구현 파일들만 쓴다.
// D3D12Graphics에도 같은 모양의 COM 참조·DXGI 형식 표가 있다. D3D 공용 도우미 모듈로 합치는 것은
// 후속이다(좌표 전환 C7이 D3D12Device.cpp를 고치는 동안 그 파일을 건드리지 않으려고 미뤘다).
namespace rocks::graphics::d3d11 {

// 기록 중 그릴 수 있는 스왑체인 버퍼 수다(flip model 최소 2).
inline constexpr uint32 SwapChainBufferCount = 2;
// 서로 다른 SamplerDesc 조합 수의 상한이다(D3D12와 같다. D3D11 자체 한도는 4096개).
inline constexpr uint32 SamplerCapacity = 256;
inline constexpr uint32 MaxMipLevels = 16;
// 2D 텍스처 한 변의 최대 크기다(feature level 11_0).
inline constexpr uint32 MaxTextureExtent = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
// 업로드 ring 안 원본 정렬이다. UpdateSubresource는 원본 정렬을 요구하지 않으므로 memcpy 효율만
// 본다.
inline constexpr uint64 UploadAlignment = 16;
// 소멸 때까지 놓지 않을 회수 항목의 fence 값이다. 정상 동작 중의 완료 값은 이 값에 닿지 않는다.
inline constexpr uint64 RetainUntilDestructionFence = NumericLimits<uint64>::Max();
// 장치 제거 뒤 fence가 돌려주는 완료 값이다(ID3D11Fence::GetCompletedValue).
inline constexpr uint64 RemovedFenceValue = NumericLimits<uint64>::Max();
// timestamp query 객체 수(프레임 slot × slot당 한도)다.
inline constexpr uint32 TimestampQueryCount = TimestampFrameSlots * MaxTimestampsPerFrame;
// CalibrateClocks가 disjoint 구간을 만나면 다시 재는 최대 횟수다(UE CalibrateTimers와 같다).
inline constexpr uint32 MaxCalibrationAttempts = 10;
// 회수 항목 하나가 쥐는 COM 객체 수의 상한이다(텍스처: 자원·SRV·RTV, 파이프라인: VS·PS·입력
// layout, 스왑체인: 스왑체인·버퍼·RTV).
inline constexpr usize MaxRetiredObjects = 3;
// dxgidebug.h의 DXGI_DEBUG_DXGI다(dxguid.lib 없이 쓰려고 값을 직접 둔다).
inline constexpr GUID DxgiDebugDxgi = {
    0x25cddaa4, 0xb1c6, 0x47e1, {0xac, 0x3e, 0x98, 0x87, 0x5b, 0x5a, 0x2e, 0x2a}};

// COM 객체 참조다. 소멸 시 Release한다(예외 없이 쓰는 최소 RAII).
template <class T>
class ComRef {
public:
    ComRef() noexcept = default;

    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            Reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }

        return *this;
    }

    ~ComRef() {
        Reset();
    }

    void Reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    T* Get() const noexcept {
        return pointer_;
    }

    T* operator->() const noexcept {
        return pointer_;
    }

    // 출력 인자로 넘길 주소다. 기존 참조는 먼저 해제한다.
    T** Out() noexcept {
        Reset();
        return &pointer_;
    }

    // 소유권을 넘겨준다(참조 수는 그대로다).
    T* Detach() noexcept {
        T* pointer = pointer_;
        pointer_ = nullptr;
        return pointer;
    }

    // 이미 참조를 하나 가진 포인터의 소유를 넘겨받는다.
    void Attach(T* pointer) noexcept {
        Reset();
        pointer_ = pointer;
    }

    explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

private:
    T* pointer_ = nullptr;
};

// 진단 문자열 조립 칸이다(셰이더 입력 대조 실패, HRESULT가 붙은 실패 설명). 넘치면 자른다.
class Diagnostic {
public:
    Diagnostic& Text(const char* text) noexcept {
        while (*text != '\0') {
            Put(*text++);
        }

        return *this;
    }

    Diagnostic& Number(uint32 value) noexcept {
        char digits[12] = {};
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            Put(digits[--count]);
        }

        return *this;
    }

    // "0x887A0005" 모양의 16진 8자리다(HRESULT).
    Diagnostic& Hex(uint32 value) noexcept {
        constexpr char Digits[] = "0123456789ABCDEF";
        Text("0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            Put(Digits[(value >> shift) & 0xFu]);
        }

        return *this;
    }

    const char* CStr() const noexcept {
        return buffer_;
    }

    usize Length() const noexcept {
        return length_;
    }

    bool Truncated() const noexcept {
        return truncated_;
    }

private:
    void Put(char letter) noexcept {
        if (length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = letter;
            buffer_[length_] = '\0';
        } else {
            truncated_ = true;
        }
    }

    char buffer_[512] = {};
    usize length_ = 0;
    bool truncated_ = false;
};

// RGBA8 텍스처 형식의 DXGI 형식이다. 모르는 형식은 UNKNOWN이다.
DXGI_FORMAT ResourceFormat(Format format) noexcept;

// 정점 원소 형식의 DXGI 형식이다(RenderBase 10.4절 표, UE D3D11VertexDeclaration.cpp).
DXGI_FORMAT AttributeFormat(VertexFormat format) noexcept;

// HLSL 시맨틱은 대소문자를 가리지 않는다. 정점 입력 시맨틱이 ATTRIBUTE인지 본다.
bool IsAttributeSemantic(const char* name) noexcept;

D3D11_FILTER SamplerFilter(Filter filter) noexcept;
D3D11_TEXTURE_ADDRESS_MODE SamplerAddress(AddressMode mode) noexcept;

// 장치 제거·재설정을 뜻하는 HRESULT인지다.
bool IsDeviceRemovedResult(HRESULT result) noexcept;

// text를 capacity byte 칸에 NUL로 끝나게 복사한다. 잘렸으면 true다.
bool CopyTruncated(char* out, usize capacity, const char* text) noexcept;

} // namespace rocks::graphics::d3d11
