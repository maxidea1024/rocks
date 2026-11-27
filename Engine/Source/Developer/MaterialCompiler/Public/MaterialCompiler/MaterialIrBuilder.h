#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrCustomTable.h>
#include <MaterialCompiler/MaterialIrParameter.h>
#include <MaterialCompiler/MaterialIrTexture.h>
#include <MaterialCompiler/MaterialIrTextureSample.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 머티리얼 IR 빌더다(Docs/Design/MaterialSystem 6.5·6.8절). 값을 만들 때 (타입, 연산, 피연산자,
// 즉시값)이 같은 값을 찾아 그 번호를 돌려준다(hash-consing — CSE). 피연산자가 모두 상수면 정확히
// 반올림되는 연산만 그 자리에서 접고(6.8절 범위), 정해진 대수 단순화(x*1·x/1·x-0·정수 x+0·swizzle
// 합치기)를 한다. 타입 규칙을 어긴 요청은 InvalidArgument다: lowering이 암묵 변환(5.3절)과 V1
// 진단을 먼저 하고 맞는 타입으로 부른다.
//
// 결정성: 값 번호는 만든 순서다. 해시 표는 찾기에만 쓰고 순회하지 않는다(6.6절).
// 스레드: 빌더 하나를 여러 스레드가 함께 쓰지 않는다.
//
// 사용 예:
//   IrBuilder builder(allocator);
//   builder.SetSite(IrSite{node, "Result"});
//   auto one = builder.FloatConstant(Span<const float>({1.0f}));
//   auto sum = builder.Binary(IrOp::Add, x, one.Value());
namespace rocks::material {

class ROCKS_MATERIALCOMPILER_API IrBuilder {
public:
    explicit IrBuilder(Allocator& allocator) noexcept;

    IrBuilder(const IrBuilder&) = delete;
    IrBuilder& operator=(const IrBuilder&) = delete;

    // 다음에 새로 만드는 값의 원인 위치다(이미 있는 값을 돌려줄 때는 바꾸지 않는다).
    void SetSite(const IrSite& site) noexcept {
        site_ = site;
    }

    // 상수다. 성분 수는 1–4이고, 오류: 성분 수 범위 밖이면 InvalidArgument, 할당 실패.
    Result<IrValueId> FloatConstant(Span<const float> components) noexcept;
    Result<IrValueId> IntConstant(Span<const int32> components) noexcept;
    Result<IrValueId> UIntConstant(Span<const uint32> components) noexcept;
    Result<IrValueId> BoolConstant(Span<const bool> components) noexcept;
    Result<IrValueId> DoubleConstant(Span<const double> components) noexcept;

    // 연산이다. 피연산자 타입 규칙(6.8절 — 같은 타입, 비교는 Bool 결과 등)을 어기면
    // InvalidArgument다.
    Result<IrValueId> Unary(IrOp op, IrValueId operand) noexcept;
    Result<IrValueId> Binary(IrOp op, IrValueId left, IrValueId right) noexcept;
    Result<IrValueId> Ternary(IrOp op, IrValueId first, IrValueId second, IrValueId third) noexcept;

    // 스칼라 operand를 components(1–4)개로 복제한다(1이면 operand 그대로).
    Result<IrValueId> Splat(IrValueId operand, uint8 components) noexcept;
    // 벡터 operand의 성분을 고른다(1–4개, 각 번호 < 성분 수). 같은 성분 그대로면 operand다.
    Result<IrValueId> Swizzle(IrValueId operand, Span<const uint8> components) noexcept;
    // 같은 스칼라 종류의 두 벡터를 이어 붙인다(성분 합 ≤ 4).
    Result<IrValueId> Append(IrValueId left, IrValueId right) noexcept;
    // 스칼라 종류를 바꾼다(행렬 제외). 같은 종류면 operand다.
    Result<IrValueId> Convert(IrValueId operand, IrScalar target) noexcept;
    // condition(Bool — 스칼라 또는 값과 같은 성분 수)이 참인 성분은 whenTrue, 거짓은 whenFalse다.
    Result<IrValueId> Select(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse) noexcept;
    // Select와 같은 뜻의 동적 분기다(조건은 Bool 스칼라).
    Result<IrValueId> Branch(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse) noexcept;
    // 문맥 값 표 index의 값이다(6.9절). type·frequency는 표에서 lowering이 정한다(빈도는 인자
    // 빈도와의 최대가 된다). 접지 않는다. 오류: type이 규칙 밖이거나 인자가 MaxIrOperands개를
    // 넘거나 빌더 밖이면 InvalidArgument.
    Result<IrValueId> Extern(uint32 index, IrExternVariant variant, IrType type,
                             IrFrequency frequency, Span<const IrValueId> arguments) noexcept;
    // 정점 값 vertex를 픽셀로 보간한 값이다(6.9절 MS2b4a — Float 벡터·스칼라만, 상수는 그 상수).
    // 오류: Float가 아니거나 빌더 밖이면 InvalidArgument.
    Result<IrValueId> Interpolate(IrValueId vertex) noexcept;
    // 늦은 진단의 자리 값이다(6.10절 MS2c1). node마다 다른 값이다(hash-consing으로 합쳐지지
    // 않는다). 오류: type이 규칙 밖이면 InvalidArgument.
    Result<IrValueId> Pending(IrType type, IrPendingKind kind, uint8 prerequisite,
                              Guid node) noexcept;
    // 수 파라미터 읽기다(6.10절 MS2c2). 같은 ParameterId는 한 값·한 표 항목이다(처음 기본값이
    // 남는다). 빈도 Uniform. 오류: type이 종류와 맞지 않으면 InvalidArgument.
    Result<IrValueId> Parameter(const IrParameter& parameter) noexcept;
    // 파라미터 컬렉션 벡터 하나다(Float4, 빈도 Instance). 오류: index ≥ vectors면 InvalidArgument.
    Result<IrValueId> CollectionValue(Guid collection, uint32 index, uint32 vectors) noexcept;

    // 텍스처 값이다(6.10절 MS2c3). 같은 (출처, id)는 한 표 항목이다. samplerType은 객체 노드의 샘플
    // 종류다(TextureObject 입력으로 받은 샘플이 쓴다). 빈도 Uniform. 오류: 같은 (출처, id)의 종류가
    // 다르거나 kind가 None이면 InvalidArgument.
    Result<IrValueId> Texture(const IrTexture& texture, uint8 samplerType) noexcept;
    // 텍스처 샘플이다. uv는 종류에 맞는 Float 폭(2D·External 2, Cube·2DArray·3D 3, CubeArray 4),
    // Level·Bias는 first가 Float 스칼라, Gradient는 first·second가 미분 폭(uv 폭 — 2DArray는 2,
    // CubeArray는 3)의 Float다. 결과 Float4. 오류: 규칙 밖이면 InvalidArgument.
    Result<IrValueId> TextureSample(const IrTextureSampleDesc& desc, IrValueId texture,
                                    IrValueId uv, IrValueId first, IrValueId second) noexcept;
    // 표면 값이다(6.11절 MS2d1 — record는 번역기 표면 기록 번호). 기록마다 다른 값이다.
    Result<IrValueId> SurfaceRef(uint32 record) noexcept;
    // 속성 집합 값이다(6.12절 MS2e1 — record는 번역기 속성 기록 번호).
    Result<IrValueId> AttributesRef(uint32 record) noexcept;
    // 라이브러리 함수 호출이다(6.13절 MS2f3). parameters는 즉시값 [1]–[3](최대 3), arguments는 실행
    // 값(최대 MaxIrOperands)이다. 접지 않는다. 오류: function·type이 규칙 밖이거나 개수를 넘거나
    // 인자가 빌더 밖·수 값이 아니면 InvalidArgument.
    Result<IrValueId> Library(uint32 function, Span<const uint64> parameters, IrType type,
                              Span<const IrValueId> arguments) noexcept;
    // Custom HLSL 함수 호출이다(6.13절 MS2f5). function은 CustomFunctions() 번호, arguments는 그
    // 항목의 인자 순서·형 그대로다(3개씩 CustomArguments 사슬로 잇는다). 오류: 번호가 표 밖이거나
    // 인자 수·형이 항목과 다르면 InvalidArgument, 할당 실패.
    Result<IrValueId> CustomCall(uint32 function, Span<const IrValueId> arguments) noexcept;
    // CustomCall call의 추가 출력 output이다. 오류: call이 CustomCall이 아니거나 output이 항목의
    // 추가 출력 수 밖이면 InvalidArgument.
    Result<IrValueId> CustomOutput(IrValueId call, uint32 output) noexcept;
    // 텍스처 slot의 논리 크기(texel이면 1/크기)다. 2DArray·Texture3D는 Float3, 2D는 Float2다.
    // 오류: 다른 종류이거나 텍스처가 아니면 InvalidArgument.
    Result<IrValueId> TextureSize(IrValueId texture, bool texel) noexcept;
    // Double 값을 성분마다 주소 모드(modes — 2 bit씩, 성분 0이 하위)로 Float로 낮춘다.
    Result<IrValueId> AddressDemote(IrValueId operand, uint32 modes) noexcept;

    // 텍스처 표다(Texture 값의 즉시값 [3]이 가리킨다).
    Span<const IrTexture> Textures() const noexcept {
        return textures_.View();
    }

    // Custom HLSL 함수 표다(CustomCall 즉시값 [0]이 가리킨다 — lowering이 항목을 만든다).
    IrCustomTable& CustomFunctions() noexcept {
        return customs_;
    }

    const IrCustomTable& CustomFunctions() const noexcept {
        return customs_;
    }

    // 파라미터 표다(Parameter 값의 즉시값 [3]이 가리킨다).
    Span<const IrParameter> Parameters() const noexcept {
        return parameters_.View();
    }

    const IrValue& Value(IrValueId id) const noexcept;

    Span<const IrValue> Values() const noexcept {
        return values_.View();
    }

    usize Count() const noexcept {
        return values_.Size();
    }

    // 상수 값이 모든 성분에서 value(float·double) 또는 그 정수인가(단순화 판정 — 6.8절).
    bool IsConstantEqualTo(IrValueId id, int32 value) const noexcept;

private:
    struct Key {
        IrValue value;
    };

    struct KeyHash {
        uint64 operator()(const Key& key) const noexcept;
    };

    struct KeyEqual {
        bool operator()(const Key& left, const Key& right) const noexcept;
    };

    Result<IrValueId> Intern(IrValue value) noexcept;
    Result<IrValueId> Fold(const IrValue& value) noexcept;
    bool IsConstant(IrValueId id) const noexcept;
    bool IsValid(IrValueId id) const noexcept;
    // 수 값(텍스처가 아닌 값)인가.
    bool IsNumeric(IrValueId id) const noexcept;

    Array<IrValue> values_;
    Array<IrParameter> parameters_;
    Array<IrTexture> textures_;
    IrCustomTable customs_;
    HashMap<Key, IrValueId, KeyHash, KeyEqual> lookup_;
    IrSite site_;
};

} // namespace rocks::material
