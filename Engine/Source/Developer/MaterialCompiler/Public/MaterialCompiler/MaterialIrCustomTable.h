#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrCustomFunction.h>
#include <MaterialCompiler/MaterialIrCustomItem.h>

// Custom HLSL 함수 표다(Docs/Design/MaterialSystem 6.13절 MS2f5). 빌더가 lowering 동안 채우고,
// 모듈은 진입점에서 닿는 CustomCall의 항목만 처음 닿은 순서로 옮겨 담는다(생성 원문
// MaterialCustom_<NodeId>_<번호>의 번호). 문자열은 표가 소유한다(번역 입력 문서를 빌리지 않는다 —
// 번역 결과가 입력보다 오래 산다).
//
// 항목은 Begin → Add* → End로 만든다. End는 (node, context, 인자 형)이 같은 항목이 이미 있으면
// 새로 쓴 칸·문자를 버리고 그 번호를 돌려준다(UE CustomExpression의 같은 노드·같은 입력 재사용).
//
// 사용 예:
//   IrCustomTable& table = builder.CustomFunctions();
//   if (auto begun = table.Begin(node, IrCustomContext::Pixel, IrFloat(3), code, false); !begun)
//   {...} (void)table.AddArgument("UV", IrFloat(2)); auto index = table.End();
namespace rocks::material {

class ROCKS_MATERIALCOMPILER_API IrCustomTable {
public:
    explicit IrCustomTable(Allocator& allocator) noexcept
        : functions_(allocator), items_(allocator), text_(allocator) {
    }

    IrCustomTable(const IrCustomTable&) = delete;
    IrCustomTable& operator=(const IrCustomTable&) = delete;

    Span<const IrCustomFunction> Functions() const noexcept {
        return functions_.View();
    }

    // 표의 문자열이다(범위가 저장소 밖이면 빈 문자열).
    StringView Text(IrText text) const noexcept;

    // 항목의 목록 칸이다(인자 → 추가 출력 → 정의 → include).
    Span<const IrCustomItem> Arguments(const IrCustomFunction& function) const noexcept;
    Span<const IrCustomItem> Outputs(const IrCustomFunction& function) const noexcept;
    Span<const IrCustomItem> Defines(const IrCustomFunction& function) const noexcept;
    Span<const IrCustomItem> Includes(const IrCustomFunction& function) const noexcept;

    // 새 항목을 연다. code는 본문 원문(return 감싸기 전 — Begin이 UE 규칙으로 감싸고 \r\n을 \n으로
    // 바꾼다)이다. 오류: 이미 연 항목이 있거나 returnType이 Float1–4가 아니면 InvalidArgument, 할당
    // 실패.
    Result<void> Begin(Guid node, IrCustomContext context, IrType returnType, StringView code,
                       bool pixelDiscard) noexcept;
    // 연 항목에 칸을 더한다(넣은 순서 — 목록 순서가 섞이면 InvalidArgument: 인자 → 출력 → 정의 →
    // include). 오류: 연 항목이 없거나 형이 규칙 밖(인자는 수 값·Double·텍스처, 출력은
    // Float1–4)이면 InvalidArgument, 할당 실패.
    Result<void> AddArgument(StringView name, IrType type) noexcept;
    Result<void> AddOutput(StringView name, IrType type) noexcept;
    Result<void> AddDefine(StringView name, StringView value) noexcept;
    Result<void> AddInclude(StringView path) noexcept;
    // 연 항목을 닫고 그 번호(또는 같은 기존 항목 번호)를 돌려준다. 오류: 연 항목이 없으면
    // InvalidArgument.
    Result<uint32> End() noexcept;
    // 연 항목을 버린다(lowering이 중간에 실패할 때). 연 항목이 없으면 아무 일도 하지 않는다.
    void Abandon() noexcept;

    // source의 index 항목을 이 표 끝에 복사하고 새 번호를 돌려준다(모듈 만들기 — 중복 검사 없음).
    // 오류: index가 표 밖이거나 연 항목이 있으면 InvalidArgument, 할당 실패.
    Result<uint32> CopyFrom(const IrCustomTable& source, uint32 index) noexcept;

    void Clear() noexcept {
        functions_.Clear();
        items_.Clear();
        text_.Clear();
        open_ = false;
    }

private:
    // 항목을 연다(본문 없이 — Begin·CopyFrom이 본문을 쓴다).
    Result<void> Open(Guid node, IrCustomContext context, IrType returnType,
                      bool pixelDiscard) noexcept;
    Result<IrText> Store(StringView text) noexcept;
    Result<void> AddItem(uint32 list, IrCustomItem item) noexcept;
    Span<const IrCustomItem> List(const IrCustomFunction& function, uint32 skip,
                                  uint32 count) const noexcept;

    Array<IrCustomFunction> functions_;
    Array<IrCustomItem> items_;
    Array<char> text_;
    // 연 항목이다(open_이면 functions_ 끝이 아니라 여기 있다).
    IrCustomFunction pending_;
    // 연 항목이 시작한 칸·문자 위치다(End·Abandon이 되돌린다).
    usize pendingItems_ = 0;
    usize pendingText_ = 0;
    // 연 항목에 마지막으로 더한 목록 번호다(0 인자, 1 출력, 2 정의, 3 include).
    uint32 pendingList_ = 0;
    bool open_ = false;
};

} // namespace rocks::material
