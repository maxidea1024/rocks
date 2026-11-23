#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringView.h>
#include <NodeGraph/GraphDocument.h>

// 문서에 나오는 Name 토큰을 원문 정규 순서의 순위로 바꾸는 내부
// 표다(Docs/Design/NodeGraph 6.1절 "이름 키"). 정렬 비교마다 pool을 조회하지 않도록, 서로
// 다른 토큰마다 한 번만 원문을 읽어 순위를 매긴다.
//
// 사용 예:
//   GraphNameRanks ranks(scratch, names);
//   ranks.Add(node.type);              // 문서의 모든 Name을 더한다
//   ranks.Build();
//   ranks.Rank(node.type);             // 원문 순위(같은 Name이면 같은 순위)
//
// 수명: Build가 얻은 원문 lease를 표가 쥔다. 표는 pool Shutdown 전에 파괴한다.
namespace rocks::nodegraph::detail {

// Rank가 표에 없는 토큰에 돌려주는 값이다.
inline constexpr uint32 UnrankedName = NumericLimits<uint32>::Max();

class GraphNameRanks {
public:
    // skipInvalid가 참이면 구조가 잘못된 토큰을 오류 없이 건너뛴다(Rank는 UnrankedName). 불변식
    // 검사가 그 토큰을 GI11 위반으로 보고하도록 쓴다.
    GraphNameRanks(Allocator& allocator, NamePool& names, bool skipInvalid = false) noexcept;

    GraphNameRanks(const GraphNameRanks&) = delete;
    GraphNameRanks& operator=(const GraphNameRanks&) = delete;

    // 토큰 하나를 더한다(중복 허용). 구조가 잘못된 토큰은 InvalidArgument(skipInvalid면 건너뜀),
    // 할당 실패는 그 오류다.
    Result<void> Add(Name name) noexcept;

    // 문서의 모든 Name(도메인·노드 종류·입력 기본값·고아 핀·간선 핀)을 더한다.
    Result<void> AddDocument(const GraphDocument& document) noexcept;

    // 중복을 지우고 원문을 읽어 순위를 매긴다. 한 번만 부른다. names 조회 오류·할당 실패를
    // 돌려준다.
    Result<void> Build() noexcept;

    // 원문 순위다(Build 뒤). 같은 Name은 같은 순위다. 표에 없으면 UnrankedName이다.
    uint32 Rank(Name name) const noexcept;

    // 원문이다(Build 뒤, 표가 사는 동안). 표에 없으면 빈 view다.
    StringView Text(Name name) const noexcept;

private:
    struct TokenKey {
        uint8 normal = 0;
        uint64 id = 0;
        uint32 number = 0;
    };

    struct Entry {
        TokenKey key;
        Name name;
        uint32 rank = 0;
        uint32 lease = 0;
    };

    static TokenKey KeyOf(Name name) noexcept;
    static int32 CompareKeys(const TokenKey& left, const TokenKey& right) noexcept;
    usize Find(Name name) const noexcept;

    NamePool& names_;
    Array<Entry> entries_;
    Array<NameLease> leases_;
    bool built_ = false;
    bool skipInvalid_ = false;
};

// Name 원문을 의미 바이트의 정규 형태(base ASCII 소문자, None은 빈 base·번호 0)로 나눈다. lowered에
// base를 쓴다(먼저 비운다). 할당 실패를 돌려준다.
Result<uint32> CanonicalNameParts(StringView text, Array<char>& lowered) noexcept;

} // namespace rocks::nodegraph::detail
