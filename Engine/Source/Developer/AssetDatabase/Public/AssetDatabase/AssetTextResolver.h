#pragma once

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <ReflectionText/TextReference.h>

// 에셋 참조 텍스트 resolver다(Docs/Design/AssetSystem.ko.md 4.1·4.2절·7.1절 15 A2c3,
// Docs/Design/ReflectionExtensions.ko.md 10.3절). asset-new·asset-set의 참조 값 입력과 asset-info의
// 참조 값 출력이 쓴다(TextCodecOptions::resolver).
//
// 텍스트: `<논리 경로>[.<객체 경로>]` 또는 `<AssetId 36자>[.<객체 경로>]`. 논리 경로·객체 경로
// 구성 요소에는 `.`가 없으므로 첫 `.`에서 나눈다(`/Game/Hero.Body.Socket` = 패키지 `/Game/Hero`의
// 객체 경로 `Body.Socket`). 객체 경로가 없으면 대표 에셋이다. null은 codec이 `none`으로 처리한다.
namespace rocks {

class AssetTextResolver final : public reflection_text::TextReferenceResolver {
public:
    // set·index는 빌린다(resolver가 쓰이는 동안 살아 있어야 한다 — index는 set의 설정과 같은
    // 색인이다). allocator는 경로 해석의 임시 할당에 쓴다. warnings가 있으면 Hard·Weak 해석이 연
    // 패키지의 경고(EditorPackageSet::Open)를 붙인다.
    AssetTextResolver(Allocator& allocator, EditorPackageSet& set, const AssetIndex& index,
                      Array<EditorWarning>* warnings = nullptr) noexcept;

    // Weak 참조를 풀 패키지(대입 대상 객체의 소속 패키지 — 열려 있어야 한다)를 정한다. 저장은
    // WeakObjectRef를 같은 패키지 안 대상만 보존하므로(5.1절 — weak import 없음) 그 밖의 Weak
    // 텍스트는 거부한다. null이면(기본) 모든 Weak 텍스트를 거부한다(asset-new — 새 패키지의 객체는
    // 아직 공개 전이라 찾을 수 없다).
    void SetWeakScope(AssetId package) noexcept {
        weakScope_ = package;
    }

    // text를 kind의 값으로 푼다.
    //   Hard: 대상 패키지를 집합에 연다(이미 열렸거나 저장하지 않은 새 패키지면 그대로 —
    //   EditorPackageSet::Open이 Hard·EditorOnly 의존 닫힘째 연다). 객체 경로의 객체를 찾아
    //   out.reference에 둔다. 대상 타입이 expected(0이면 Object)의 파생이 아니면 TypeMismatch다.
    //   Weak: 대상 패키지가 SetWeakScope의 패키지여야 하고(아니면 InvalidArgument — 열지 않는다,
    //   그 패키지가 열려 있지 않으면 NotFound) 그 밖은 Hard와 같다.
    //   Soft: 열지 않는다. 논리 경로는 색인(또는 저장하지 않은 새 패키지)에서 AssetId만 얻고,
    //   AssetId 텍스트는 그대로 쓴다(색인에 없어도 된다). out.guid·out.string(객체 경로)을 채운다.
    // 오류(codec이 진단 Unresolved로 바꾼다 — OutOfMemory는 호출 실패로 전파된다): 텍스트 형식
    // (논리 경로 규칙·36자 소문자 AssetId·null AssetId·빈 객체 경로·객체 경로 구성 요소 규칙)
    // InvalidArgument, 범위 밖 Weak InvalidArgument, 색인에 없는 논리 경로 NotFound, 같은 경로 키
    // 둘 AlreadyExists, 열기 실패는 EditorPackageSet::Open의 오류, 없는 객체 NotFound, 타입
    // TypeMismatch(TypeRegistry가 닫혔으면 타입 조회의 ShuttingDown), 할당 실패.
    Result<void> Resolve(StringView text, TypeId expected, reflection_text::TextReferenceKind kind,
                         FieldValue& out) noexcept override;

    // value의 텍스트를 쓴다: 패키지의 논리 경로(저장하지 않은 새 패키지는 그 경로, 그 밖은 색인
    // 경로 — 없거나 같은 id가 둘이면 36자 AssetId) 뒤에 객체 경로가 있으면 `.<객체 경로>`. Hard·
    // Weak는 handle의 소속 패키지(outermost)와 객체 경로를 Registry에서 읽는다.
    // 오류: Package 객체 자신(텍스트로 나타낼 수 없다 — 빈 객체 경로는 대표 에셋이다)은
    // Unsupported, Registry 조회의 오류(사라진 handle 등), 할당 실패.
    Result<void> Format(reflection_text::TextReferenceKind kind, const FieldValue& value,
                        StringBuilderBase& out) noexcept override;

private:
    // 참조 텍스트의 패키지 부분(논리 경로 또는 AssetId)을 id로 푼다(AssetId 텍스트는 색인을 보지
    // 않는다).
    Result<AssetId> ResolveId(StringView head) noexcept;
    // id 패키지를 집합에 열고 Package handle을 준다(열려 있으면 그대로).
    Result<ObjectHandle> OpenPackage(AssetId id) noexcept;
    // id 패키지의 경로 텍스트(없으면 36자 AssetId)를 out 뒤에 붙인다.
    Result<void> AppendPackage(AssetId id, StringBuilderBase& out) noexcept;

    Allocator* allocator_;
    EditorPackageSet* set_;
    const AssetIndex* index_;
    Array<EditorWarning>* warnings_;
    AssetId weakScope_{};
};

} // namespace rocks
