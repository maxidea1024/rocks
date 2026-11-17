#pragma once

#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCategory.h>

namespace rocks::graphics {

// 그래픽 계층의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). backend별 카테고리는 이 아래
// 이름이라 SetLevel("Graphics")로 함께 조절된다.
// - LogGraphics: backend를 고르는 호출자(장치 생성 fallback Warning, 모든 후보 실패 Error).
// - LogGraphicsD3D12·LogGraphicsD3D11: 각 backend 장치(생성 Info, 장치 제거 Error).
// - LogGraphicsD3D12Validation·LogGraphicsD3D11Validation: debug layer 메시지(메시지 ID마다 한 번
//   Warning).
// - LogShader: 셰이더 컴파일 실패를 결정하는 호출자(파이프라인을 만드는 쪽).
//
// 사용 예:
//   Log::Warning(LogGraphics, "{Backend} device creation failed at {Stage}: {Error}", ...);
inline constexpr LogCategory LogGraphics{"Graphics"};
inline constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
inline constexpr LogCategory LogGraphicsD3D12Validation{"Graphics.D3D12.Validation"};
inline constexpr LogCategory LogGraphicsD3D11{"Graphics.D3D11"};
inline constexpr LogCategory LogGraphicsD3D11Validation{"Graphics.D3D11.Validation"};
inline constexpr LogCategory LogShader{"Shader"};

// 검증 계층 메시지를 ID마다 한 번만 남기려고 기억하는 ID 수다. 넘친 뒤의 새 ID는 남기지
// 않는다(개수는 ValidationErrorCount가 센다).
inline constexpr uint32 MaxLoggedValidationIds = 64;

// 로그에 싣는 셰이더 컴파일 진단의 최대 byte 수다(넘치면 자른다).
inline constexpr usize MaxShaderDiagnosticLogBytes = 1024;

// 검증 메시지 ID를 처음 볼 때만 참을 돌려준다(메시지 ID마다 Warning 한 번). 동기화하지 않으므로
// 검증 메시지를 비우는 쪽이 직렬화한다. 기억 칸(MaxLoggedValidationIds)이 차면 새 ID도 거짓이다.
//
// 사용 예:
//   if (filter.First(message->ID)) { Log::Warning(LogGraphicsD3D12Validation, ...); }
class ValidationMessageFilter {
public:
    bool First(uint32 id) noexcept {
        for (uint32 index = 0; index < count_; ++index) {
            if (ids_[index] == id) {
                return false;
            }
        }

        if (count_ == MaxLoggedValidationIds) {
            // 표가 찬 첫 순간에 한 번 알린다(그 뒤의 새 ID는 세기만 한다).
            if (!overflowReported_) {
                overflowReported_ = true;
                Log::Warning(
                    LogGraphics,
                    "Validation message ID table is full ({Count} IDs); further validation "
                    "message IDs are counted but not logged",
                    MaxLoggedValidationIds);
            }

            return false;
        }

        ids_[count_++] = id;
        return true;
    }

private:
    uint32 ids_[MaxLoggedValidationIds]{};
    uint32 count_ = 0;
    bool overflowReported_ = false;
};

} // namespace rocks::graphics
