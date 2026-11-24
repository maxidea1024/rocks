#pragma once

#include <AssetCooker/CookConfig.h>
#include <AssetCooker/CookTargets.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetKey.h>
#include <AssetRuntime/PackageWriter.h>
#include <Object/TypeRegistry.h>

// 쿠킹 처리기와 처리기 표다(Docs/Design/AssetSystem 10.2절 결정 6, "A4c 구현 세부"). 처리기는
// 대표 에셋 타입마다 쿠킹 출력을 만든다(기본 처리기는 에디터 객체 그래프를 그대로 쿠킹 패키지로 —
// A4c2). 정체성(처리기 Guid·버전 Guid)과 설정 키가 build key에 들어간다.
//
// 사용 예:
//   class TextureCookProcessor final : public CookProcessor { ... };
//   CookProcessorTable table(allocator, types, defaultProcessor);
//   table.Register(TypeIdOf<Texture2D>(), textureProcessor);
//   auto processor = table.Find(entry.type);   // 가장 가까운 등록 처리기, 없으면 기본 처리기
namespace rocks {

// 처리기 하나의 쿠킹 문맥이다(실행기가 채운다 — A4c2). 포인터는 Cook 동안만 유효하다.
struct CookContext {
    Allocator* allocator = nullptr;
    // 열린 에디터 패키지의 Registry·접근 문맥·Package handle과 그 색인 항목이다.
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    ObjectHandle package{};
    const AssetIndexEntry* entry = nullptr;
    // 대상 프로파일과 그 출력 설정 SHA-256, 쿠커 버전이다(쿠킹 Summary 값).
    const TargetProfile* profile = nullptr;
    Sha256Digest profileSettings{};
    uint32 cookerVersion = 0;
    // 에디터 패키지의 bulk 원본이다(열린 패키지의 파일 bulk 저장소).
    PackageBulkSource* bulkSource = nullptr;
    // 너겟 캐시다(없으면 null — 기본 처리기는 쓰지 않는다, 텍스처 처리기 A5가 쓴다).
    NuggetCache* nuggets = nullptr;
    // 참이면 처리기는 너겟 적중을 쓰지 않고 다시 만든 결과를 적중 결과와 byte로 대조하고, 다르면
    // failure 코드 NuggetMismatch(오류 InvalidArgument)로 실패한다(10.2절 결정 12 — A4e1).
    bool verifyNuggets = false;
    // 쿠킹 패키지 byte를 받는 곳과 실패 진단이다.
    PackageSink* sink = nullptr;
    AssetDiagnostic* failure = nullptr;
    // 처리기 경고를 받는 곳이다(null이면 버린다 — 12.14절 10번). 처리기는 warning = true·asset =
    // 대상 id로 넣고, 실행기가 실행 진단으로 옮긴다(게시를 막지 않는다). push의 할당 실패는
    // OutOfMemory로 돌려준다.
    Array<CookDiagnostic>* warnings = nullptr;
};

// 처리기다. Cook의 기본은 에디터 객체 그래프를 그대로 쿠킹 패키지로 쓴다(UE가 패키지를 대상
// 플랫폼용으로 저장하는 것과 같다 — EditorOnly 필드는 writer가 뺀다).
class ROCKS_ASSETCOOKER_API CookProcessor {
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

    // 쿠킹 패키지를 context.sink에 쓴다. 기본: WritePackage(kind Cooked, profileId·profileSettings·
    // cookerVersion, 프로파일 bulkCodec·bulkAlignment(0이면 PackageDefaultCookedBulkAlignment),
    // bulkSource). 오류: WritePackage의 오류(진단은 context.failure).
    virtual Result<void> Cook(CookContext& context) const noexcept;
};

// 기본 처리기의 정체성이다(기본 Cook의 출력 의미가 바뀌면 버전을 바꾼다).
inline constexpr NuggetProcessorId DefaultCookProcessorId{
    Guid{0x524f434b44454643ull, 0x0000000000000001ull},
    Guid{0x524f434b44454656ull, 0x0000000000000001ull}};

// 기본 처리기다(처리기 표의 fallback — Asset 루트).
class DefaultCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return DefaultCookProcessorId;
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
