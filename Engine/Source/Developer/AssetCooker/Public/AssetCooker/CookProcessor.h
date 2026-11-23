#pragma once

#include <AssetDatabase/AssetIndex.h>
#include <AssetNugget/NuggetKey.h>
#include <Object/TypeRegistry.h>

// 쿠킹 처리기와 처리기 표다(Docs/Design/AssetSystem.ko.md 10.2절 결정 6, "A4c 구현 세부"). 처리기는
// 대표 에셋 타입마다 쿠킹 출력을 만든다(기본 처리기는 에디터 객체 그래프를 그대로 쿠킹 패키지로 —
// A4c2). 정체성(처리기 Guid·버전 Guid)과 설정 키가 build key에 들어간다.
//
// 사용 예:
//   class TextureCookProcessor final : public CookProcessor { ... };
//   CookProcessorTable table(allocator, types, defaultProcessor);
//   table.Register(TypeIdOf<Texture2D>(), textureProcessor);
//   auto processor = table.Find(entry.type);   // 가장 가까운 등록 처리기, 없으면 기본 처리기
namespace rocks {

class CookProcessor {
public:
    virtual ~CookProcessor() = default;

    // 처리기 Guid와 버전 Guid다(출력 의미가 바뀌면 버전 Guid를 바꾼다 — 그 처리기의 모든 키가
    // 바뀐다).
    virtual NuggetProcessorId Identity() const noexcept = 0;

    // 처리기 설정 키를 쓴다(색인 정보만 — 패키지 안 설정은 콘텐츠 해시가 이미 담는다). 기본은
    // 아무것도 쓰지 않는다. 실패는 writer 상태로 남긴다(NuggetKeyWriter::Fail).
    virtual void WriteSettingsKey(NuggetKeyWriter& writer,
                                  const AssetIndexEntry& entry) const noexcept {
        (void)writer;
        (void)entry;
    }
};

// 대표 에셋 타입 → 처리기 표다. 처리기는 빌림이다(표보다 오래 산다). 한 스레드에서 쓴다.
class ROCKS_ASSETCOOKER_API CookProcessorTable {
public:
    // fallback은 base 사슬에 등록 처리기가 없을 때 쓰는 처리기다(보통 기본 처리기).
    CookProcessorTable(Allocator& allocator, TypeRegistry& types,
                       const CookProcessor& fallback) noexcept;

    CookProcessorTable(const CookProcessorTable&) = delete;
    CookProcessorTable& operator=(const CookProcessorTable&) = delete;

    // type의 처리기를 등록한다. 오류: 0 type InvalidArgument, 이미 등록한 type AlreadyExists, 할당
    // 실패.
    Result<void> Register(TypeId type, const CookProcessor& processor) noexcept;

    // type부터 base 사슬로 올라가 가장 가까운 등록 처리기를 준다(없으면 fallback). 오류: 사슬의
    // 타입 조회 오류(미등록 NotFound·클래스 아님 TypeMismatch·종료 뒤 ShuttingDown 등 —
    // TypeRegistry Acquire 그대로), 0 type InvalidArgument.
    Result<const CookProcessor*> Find(TypeId type) const noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

private:
    struct Entry {
        TypeId type{};
        const CookProcessor* processor = nullptr;
    };

    TypeRegistry* types_;
    const CookProcessor* fallback_;
    Array<Entry> entries_;
};

} // namespace rocks
