#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>

// 안정 ID manifest(`<패키지>.rocksids.json`, Docs/Design/SparksBytecode.ko.md 6.1절)의 편집이다
// (Docs/Design/SparksTools.ko.md 7.2절 — `sparks ids`). 규칙과 출력 byte는 메타 컴파일러
// `StableIdManifest.cs`(Register·Rename·Serialize)와 같다.
namespace rocks::sparks::tools::detail {

// C# `BoundedFile.MaximumIdentityBytes`와 같은 manifest 크기 상한이다.
inline constexpr usize MaxIdManifestBytes = usize{16} * 1024 * 1024;

// manifest 항목 하나다.
struct IdEntry {
    explicit IdEntry(Allocator& allocator) noexcept
        : kind(allocator), name(allocator), id(allocator), aliases(allocator) {
    }

    String kind;
    String name;
    String id;
    Array<String> aliases;
    bool retired = false;
};

// 멤버 kind(이름의 주인 type이 같은 manifest에 있어야 하는 것 — field·subobject·function·rpc·
// enumCase)인지다(C# `StableIdManifest.IsMember`).
bool IsMemberIdKind(StringView kind) noexcept;

// Sparks 이름 키의 주인 type 이름이다: 마지막 `/` 뒤 첫 `.`의 다음 `.` 앞까지(멤버가 없으면 이름
// 전체 — C# `StableIdManifest.Owner`의 Sparks 갈래).
StringView IdOwner(StringView name) noexcept;

// text(컴파일러가 S1006 없이 읽은 manifest)를 entries 끝에 읽어 붙인다. 값 false는 형식이 맞지
// 않음(내부 불일치), 실패(Result)는 할당 실패다.
Result<bool> ReadIdEntries(Allocator& allocator, StringView text, Array<IdEntry>& entries) noexcept;

// 활성 항목 (kind, from)의 이름을 to로 바꾸고 from을 aliases에 더한다. type이면 주인이 from인 멤버
// 항목(은퇴 포함)도 주인 부분을 to로 바꾸고 옛 이름을 aliases에 더한다(C# `Rename`). 값 false는
// 활성 항목이 없음, 멤버의 주인이 바뀜, 새 이름이 같은 kind의 이름·alias로 이미 있음이다(C#
// `Validate`가 거부할 manifest를 만들지 않는다 — 항목은 바뀌지 않는다). 실패(Result)는 할당 실패다.
Result<bool> RenameIdEntry(Allocator& allocator, Array<IdEntry>& entries, StringView kind,
                           StringView from, StringView to) noexcept;

// 새 활성 항목(aliases 없음)을 더한다(C# `Register`). 값 false는 같은 kind의 이름·alias로 이미 있음
// (더하지 않는다), 실패(Result)는 할당 실패다.
Result<bool> AddIdEntry(Allocator& allocator, Array<IdEntry>& entries, StringView kind,
                        StringView name, StringView id) noexcept;

// manifest 원문을 out에 붙인다: `{"schema": 1, "entries": [...]}`, 항목은 (kind, name) byte 순,
// aliases는 byte 순, 들여쓰기 2칸·LF·끝 LF(C# `Serialize`와 같은 byte). 실패: 할당 실패, 잘못된
// UTF-8(InvalidArgument).
Result<void> AppendIdManifest(Allocator& allocator, StringBuilderBase& out,
                              Span<const IdEntry> entries) noexcept;

} // namespace rocks::sparks::tools::detail
