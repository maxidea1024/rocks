#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <RenderBase/ShaderResourceMap.h>

// 묶음 설정 ParameterBatch의 자원 부분(Docs/Design/RenderBase 9.3절, 9.13절 6항)이다: 자원 지도
// 하나의 값(텍스처·샘플러·버퍼 view)을 모아 검증하고 장치에 한 번에 올린다. 상수 부분은
// ConstantBlock·ApplyConstantBlock(9.11절)이다. 생성 표 지도면 값을 파라미터 구조체의 장치 자원
// 멤버에서 한 번에 읽을 수 있다(SetParameters, ShaderSystem 16.2절 SSD64).
namespace rocks::render {

// 자원 지도 하나의 값 묶음이다. 지도는 묶음보다 오래 산다.
//
// 사용 예(그리기 항목이 갖는 머티리얼 자원):
//   ParameterBatch batch(allocator, materialMap);
//   batch.Initialize();
//   batch.SetTexture("Albedo", albedo);
//   batch.SetSampler("AlbedoSampler", anisotropic);   // 없으면 텍스처 desc 샘플러
//   batch.SetBufferView("Lights", lights);
//   if (!batch.Validate(device, &reason)) { ... }      // 값을 바꾼 뒤 소유 스레드에서 한 번
//   ...
//   batch.Apply(commands);                             // 그리기마다(병렬 기록 pass 함수에서도)
//   commands.Draw(3);
//
// 생성 표 지도의 항목 가운데 셰이더가 쓰지 않는 항목(자리 없음)과 그래프 자원 멤버 항목(쓰기 자원
// 포함 — 렌더 그래프 pass 문맥이 묶는다)은 묶음의 값이 아니다: Validate·Apply가 건너뛴다.
//
// 사용 예(생성 구조체):
//   DecalParameters values;                       // ROCKS_SHADER_PARAMETERS,
//   DeviceTextureParameter 멤버 values.albedo.texture = albedoTexture; values.albedo.sampler =
//   SamplerOverride::Of(anisotropic); batch.SetParameters(values);                  // 멤버마다
//   SetTexture·SetSampler·SetBufferView
//
// 스레드: Set*·Validate는 소유 스레드, Apply는 값을 바꾸지 않는 동안 어느 기록 스레드에서나 부른다.
class ROCKS_RENDERBASE_API ParameterBatch {
public:
    ParameterBatch(Allocator& allocator, const ShaderResourceMap& map) noexcept;
    ParameterBatch(const ParameterBatch&) = delete;
    ParameterBatch& operator=(const ParameterBatch&) = delete;

    // 표 항목마다 빈 값을 만든다. 실패: 할당 실패 OutOfMemory.
    Result<void> Initialize() noexcept;

    // 텍스처(또는 Read view) 값을 준다. 실패: Initialize 전·다른 종류 항목 InvalidArgument, 이름
    // 없음 NotFound.
    Result<void> SetTexture(StringView name, graphics::TextureHandle texture) noexcept;

    // 샘플러 값을 준다(결정 R12 — 자기 샘플러는 텍스처 desc 대신, 공유 샘플러는 필수). 실패:
    // Initialize 전·다른 종류 항목·IsValidSamplerDesc 위반 InvalidArgument, 이름 없음 NotFound.
    Result<void> SetSampler(StringView name, const graphics::SamplerDesc& sampler) noexcept;

    // 샘플러 값을 지운다(자기 샘플러는 텍스처 desc로 돌아간다). 실패는 SetSampler와 같다.
    Result<void> ResetSampler(StringView name) noexcept;

    // 버퍼 view(Read) 값을 준다. 실패는 SetTexture와 같다.
    Result<void> SetBufferView(StringView name, graphics::BufferViewHandle view) noexcept;

    // 파라미터 구조체 값(values — layout의 C++ 구조체)의 장치 자원 멤버를 묶음 값으로 읽는다:
    // DeviceTextureParameter는 텍스처와 짝 샘플러(SamplerOverride가 given이 아니면 텍스처 desc로
    // 되돌림), DeviceBufferParameter는 버퍼 view다. 그래프 멤버·공유 샘플러는 건드리지 않는다.
    // 실패: Initialize 전·지도가 이 layout의 생성 표 지도가 아님·values null InvalidArgument,
    // 샘플러 desc가 IsValidSamplerDesc 위반이면 InvalidArgument(앞 멤버는 이미 바뀌었을 수 있다).
    Result<void> SetParameters(const void* values,
                               const shader::ShaderParameterLayout& layout) noexcept;

    template <class T>
    Result<void> SetParameters(const T& values) noexcept {
        return SetParameters(&values, shader::ShaderParameterLayoutOf<T>());
    }

    // 지금 값을 장치 자원과 대조한다(9.13절 6항, 소유 스레드). 실패: 값이 없는 텍스처·버퍼
    // view·공유 샘플러와 null handle InvalidArgument, 장치가 모르는 handle StaleHandle(장치 질의의
    // 오류), 종류 불일치 InvalidArgument — 텍스처는 SRV 없음·ReadWrite view·view 차원·MSAA 여부가
    // 셰이더 모양과 다름(TextureShapeAccepts)·형식 값 계열과 셰이더 원소 타입이 다름, 버퍼 view는
    // ReadWrite·종류· typed 값 계열·구조화 원소 byte(셰이더가 알릴 때)가 다름, 자기 샘플러 값의
    // 비교 여부가 짝 텍스처 desc와 다름. reason에 영어 이유(정적 문구)를 두고 failedParameter가
    // 있으면 항목 번호를 쓴다. 성공하면 지금 값을 검증한 값으로 둔다.
    Result<void> Validate(graphics::Device& device, const char** reason = nullptr,
                          usize* failedParameter = nullptr) noexcept;

    // 지도의 자리마다 SetTexture·SetSampler·SetBufferView를 부른다(표 순서, 단계 순서). Release가
    // 아닌 구성에서는 지금 값이 Validate를 통과한 값이어야 한다. 실패: Initialize 전·값이 없는
    // 텍스처·버퍼 view·공유 샘플러·null handle·(Release 밖) 검증하지 않은 값 InvalidArgument,
    // SetSampler의 실패는 그 오류다(앞 자리는 이미 묶였을 수 있다).
    Result<void> Apply(graphics::CommandContext& commands,
                       const char** reason = nullptr) const noexcept;

    // 지금 값이 Validate를 통과했다.
    bool IsValidated() const noexcept {
        return validatedVersion_ == version_;
    }

    const ShaderResourceMap& Map() const noexcept {
        return *map_;
    }

private:
    struct Value {
        bool given = false;
        SlotHandle handle;
        graphics::SamplerDesc sampler;
    };

    // 이름의 항목을 찾아 종류를 대조한다.
    Result<usize> Entry(StringView name, ResourceParameterKind kind) const noexcept;

    // 항목이 묶음의 값이 아닌가(자리 없음, 그래프 자원 멤버, 쓰기 자원).
    bool Skips(usize parameter) const noexcept;

    const ShaderResourceMap* map_;
    Array<Value> values_;
    uint64 version_ = 1;
    uint64 validatedVersion_ = 0;
};

} // namespace rocks::render
