#pragma once

#include <Core/Text/StringBuilder.h>
#include <Object/RuntimeTypeOps.h>

// 참조 값 텍스트의 resolver hook이다(리플렉션 B9i — Docs/Design/ReflectionExtensions.ko.md 10.3절).
// 참조 텍스트(논리 경로·AssetId)의 뜻은 에셋 계층(A1·A2)이 정하므로 codec은 이 hook으로 풀고 쓴다.
// `none`(null)은 codec이 먼저 처리하고 hook에 넘기지 않는다.
namespace rocks::reflection_text {

// 참조 값의 종류다.
enum class TextReferenceKind : uint8 {
    // 강한 참조(ObjectRef 필드·ObjectRefArray·ObjectRefMap 값) — 같은 Registry의 살아 있는 handle.
    Hard,
    // weak 참조(WeakRef) — 같은 Registry의 살아 있는 handle.
    Weak,
    // 소프트 참조(SoftObjectRef) — AssetId와 객체 경로만(로드하지 않는다 — AS 6.2절).
    Soft
};

// 사용 예(A2의 논리 경로 resolver):
//   class AssetPathResolver final : public TextReferenceResolver {
//   public:
//       Result<void> Resolve(StringView text, TypeId expected, TextReferenceKind kind,
//                            FieldValue& out) noexcept override;
//       Result<void> Format(TextReferenceKind kind, const FieldValue& value,
//                           StringBuilderBase& out) noexcept override;
//   };
//   options.resolver = &resolver;
class TextReferenceResolver {
public:
    // text(null이 아닌 참조 텍스트 — escape를 푼 원문)를 kind의 값으로 풀어 out에 쓴다. out.kind는
    // codec이 정해 두었다(ObjectReference·WeakObjectReference·SoftObjectReference). Hard·Weak는
    // out.reference(같은 Registry의 handle — 기대 타입 expected는 TypeId, 0이면 Object), Soft는
    // out.guid(AssetId)·out.string(객체 경로)을 채운다. Soft는 대상을 로드하지 않는다.
    // 오류: 풀 수 없는 텍스트는 아무 오류(NotFound·InvalidArgument 등 — codec이 진단 Unresolved로
    // 바꾼다), OutOfMemory는 codec 호출의 실패로 전파된다.
    // 호출 시점: Borrow·encode 고정을 쥐지 않은 상태(Prepare·Append·값 해석)에서만 부른다.
    virtual Result<void> Resolve(StringView text, TypeId expected, TextReferenceKind kind,
                                 FieldValue& out) noexcept = 0;

    // value(null이 아닌 kind의 값)의 텍스트를 out 뒤에 붙인다. 실패하면(아무 오류) codec이 Soft의
    // 객체 경로가 비어 있으면 36자 AssetId로 대신 쓰고, 그 밖은 Unsupported로 형식화를 실패한다.
    // 실패할 때 out에 붙인 앞부분은 codec이 지운다.
    virtual Result<void> Format(TextReferenceKind kind, const FieldValue& value,
                                StringBuilderBase& out) noexcept = 0;

protected:
    ~TextReferenceResolver() = default;
};

} // namespace rocks::reflection_text
