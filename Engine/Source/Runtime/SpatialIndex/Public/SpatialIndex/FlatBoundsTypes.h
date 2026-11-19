#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Shapes.h>

// FlatBoundsSet의 공개 값 타입이다(Docs/Design/FlatBounds.ko.md 4절). 좌표·단위는 UE 5.8과
// 같다(왼손 +X 전방·+Y 오른쪽·+Z 위, cm). 경계는 닫힌 집합이며 경계 접촉은 교차다.
namespace rocks::spatial {

// 원소를 가리키는 세대 번호 handle이다. 발급한 FlatBoundsSet에서만 의미가 있으며, 원소를 지우거나
// Clear하면 낡은 handle(StaleHandle)이 된다. 기본값은 빈 handle이다.
struct FlatBoundsHandle {
    SlotHandle slot;

    [[nodiscard]] constexpr bool IsNull() const noexcept {
        return slot.generation == 0;
    }

    friend constexpr bool operator==(FlatBoundsHandle left, FlatBoundsHandle right) noexcept {
        return left.slot.index == right.slot.index && left.slot.generation == right.slot.generation;
    }
};

// 가시성 비트 집합(FlatVisibility::words)과 항상 보임 bit 배열의 word 하나가 담는 bit 수다. 슬롯
// s의 bit는 word s / FlatVisibilityBitsPerWord의 bit s % FlatVisibilityBitsPerWord다.
// pageCapacity는 이 값의 배수라 page마다 출력 word가 겹치지 않는다.
inline constexpr uint32 FlatVisibilityBitsPerWord = 64;
// FlatVisibilityBitsPerWord = 2^FlatVisibilityWordShift다.
inline constexpr uint32 FlatVisibilityWordShift = 6;

static_assert(sizeof(uint64) * 8 == FlatVisibilityBitsPerWord,
              "FlatVisibility words are uint64, so a word must hold exactly 64 bits");
static_assert((1u << FlatVisibilityWordShift) == FlatVisibilityBitsPerWord,
              "FlatVisibilityWordShift must be log2 of FlatVisibilityBitsPerWord");

// 큰 원소·범위 밖 목록(5절)에 있는 원소의 슬롯 영역 시작이다. 목록 i번째 원소의 슬롯은
// FlatBoundsListSlotBase + i이고 page 원소의 슬롯은 page 색인 × pageCapacity + page 안 위치(이
// 값보다 작다)다. 목록 슬롯은 page 수가 바뀌어도 그대로다(비트 집합 안의 위치만 page 영역 뒤로
// 밀린다).
inline constexpr uint32 FlatBoundsListSlotBase = 1u << 31;

// 슬롯이 목록 영역인지다.
constexpr bool IsFlatBoundsListSlot(uint32 slot) noexcept {
    return slot >= FlatBoundsListSlotBase;
}

// 등록할 원소의 내용이다.
struct FlatBoundsDesc {
    // 월드 AABB(cm). Box::IsValid()가 참이어야 한다(NaN·무한대·min > max는 InvalidArgument). 크기 0
    // (점·면)은 유효하다.
    Box<double> bounds;
    // 뷰 mask와 AND가 0이면 보이지 않는다.
    uint32 filterMask = 0;
    // 그리기 거리 하한(cm)이다. 0이면 하한 없음. 유한·0 이상. 배율을 곱한 m에 대해 가장 먼
    // 꼭짓점까지 거리가 m 이상이어야 보인다.
    float minDrawDistance = 0.0f;
    // 그리기 거리 상한(cm)이다. 0이면 상한 없음. 유한·0 이상이고 0이 아니면 minDrawDistance 이상.
    // 배율을 곱한 M에 대해 AABB까지 최근접 거리가 M 이하여야 보인다.
    float maxDrawDistance = 0.0f;
    // 참이면 공간·거리 판정 없이 필터만 본다(UE "항상 보이는" 프록시). 경계는 그래도 유효해야 한다.
    bool alwaysVisible = false;
    // 호출자의 식별 값이다. 집합은 해석하지 않는다.
    uint64 userValue = 0;
};

// 뷰 하나의 판정 입력이다.
struct FlatBoundsView {
    // 안쪽 normal 평면 집합(프러스텀 등)이다. ConvexVolume::IsValid()여야 한다.
    ConvexVolume<double> volume;
    // 그리기 거리 기준점(cm)이다. 유한해야 한다.
    Vec3d distanceOrigin = Vec3d(0.0, 0.0, 0.0);
    // 그리기 거리 배율(UE MaxDrawDistanceScale 대응)이다. 유한·0 초과. 원소의 거리 하한·상한에
    // 곱한다.
    float distanceScale = 1.0f;
    // 0이면 빈 결과다(오류 아님).
    uint32 filterMask = 0;
};

// 변경 호출이 슬롯을 정하거나 바꾼 원소 하나다. 소비자는 이것으로 자기 슬롯 색인 배열을 고친다.
struct FlatBoundsMove {
    FlatBoundsHandle handle;
    // 원소의 새 슬롯이다(목록이면 FlatBoundsListSlotBase 이상).
    uint32 newSlot = 0;
};

// 변경 호출 하나가 슬롯을 정하거나 바꾼 원소다(최대 2개: 호출 대상 원소와 빈자리를 채운 원소).
struct FlatBoundsMoves {
    FlatBoundsMove moves[2];
    uint32 count = 0;
};

// QueryViews의 뷰별 출력이다. 저장소는 호출자가 소유한다.
struct FlatVisibility {
    // 슬롯당 1 bit다(비트 위치는 FlatBoundsReader::ForEachVisible 설명). WordCount() 이상이어야
    // 한다.
    Span<uint64> words;
    // QueryViews가 쓰는 결과 세대다. 집합이 변경되면 낡은 결과가 된다(ForEachVisible이
    // StaleHandle).
    uint64 epoch = 0;
};

// ForEachVisible이 보이는 원소마다 넘기는 정보다.
struct FlatVisibleElement {
    FlatBoundsHandle handle;
    uint32 slot = 0;
    uint64 userValue = 0;
};

// QueryViews 한 번의 계수다. 필드는 더하기만 하므로 여러 호출에 같은 구조체를 넘기면 합계가 된다.
struct FlatQueryStats {
    // 판정한 (뷰, 살아 있는 page) 쌍의 수다.
    uint64 pageViews = 0;
    // page 경계로 뷰 평면 하나에 대해 전부 바깥이라 원소 판정 없이 버린 (뷰, page) 쌍의 수다.
    uint64 rejectedPageViews = 0;
    // 필터를 통과해 평면 판정을 한 page 원소 수(뷰마다 센다)다. 4개 묶음 단위로 판정하므로 묶음
    // 안의 필터 통과 원소만 센다.
    uint64 testedElements = 0;
    // 필터를 통과한 항상 보임 원소 수(뷰마다)다.
    uint64 alwaysVisibleElements = 0;
    // 목록에서 판정한 원소 수(뷰마다)다.
    uint64 listElements = 0;
    // 보인다고 쓴 원소 수(뷰마다)다.
    uint64 visibleElements = 0;
};

// FlatBoundsSet::TryCreate의 설정이다(Docs/Design/FlatBounds.ko.md 5절). 길이는 cm다. 범위를 어기면
// TryCreate가 InvalidArgument를 돌려준다.
struct FlatBoundsConfig {
    // 타일 격자의 원점이다. 각 성분은 정수여야 한다.
    Vec3d anchor = Vec3d(0.0, 0.0, 0.0);
    // 타일 한 변이다(10.4절 규약 7). 양의 정수이고 2^31 · tileSize + |anchor| < 2^53이어야
    // 한다(타일 원점이 double로 정확하다).
    double tileSize = 204800.0;
    // AABB의 타일 상대 좌표 최대 절댓값이 이 값을 넘으면 큰 원소 목록에 둔다(5절). 양수이고 2^60
    // 이하여야 한다(저장 float가 유한하다).
    double largeRelativeExtent = 409600.0;
    // page 하나의 원소 수다. FlatVisibilityBitsPerWord(64)의 배수이고 [64, 65536]이다(page마다 출력
    // word가 겹치지 않는다).
    uint32 pageCapacity = 64;
    // 원소 수 상한이다. 1 이상, SlotArray 한도 이하. 넘으면 TryAdd가 CapacityExceeded다.
    uint32 maxElements = 1u << 22;
    // handle 세대 상한이다(SlotArray 규칙, 1 이상). 시험에서 retire를 검사하려고 낮춘다.
    uint32 maxGeneration = NumericLimits<uint32>::Max();
};

// Stats()의 결과다. 누적 계수는 생성 후(이동하면 함께 옮겨 감) 계속 늘어나며 Clear로도 지워지지
// 않는다.
struct FlatBoundsStats {
    // ---- 현재 구조 ----
    uint32 elementCount = 0;
    // page 원소 수다.
    uint32 pageElementCount = 0;
    // 큰 원소 목록과 범위 밖 목록의 원소 수다(둘은 같은 double 목록에 있다).
    uint32 largeCount = 0;
    uint32 outOfRangeCount = 0;
    uint32 tileCount = 0;
    uint32 livePageCount = 0;
    uint32 freePageCount = 0;
    // page 색인 높이(해제된 page 포함)다. 비트 집합의 page 영역은 이 값 × pageCapacity bit다.
    uint32 pageSlotCount = 0;
    // 타일 하나의 page 수 최댓값이다.
    uint32 maxPagesPerTile = 0;
    // 비트 집합 하나의 word 수다(WordCount).
    uint32 wordCount = 0;
    // page 반경 R(저장 float 상대 좌표의 최대 절댓값)의 최댓값과 평균이다.
    float maxPageRadius = 0.0f;
    double meanPageRadius = 0.0;
    // ---- 메모리(byte) ----
    usize recordBytesUsed = 0;
    usize recordBytesReserved = 0;
    // page SoA 배열(경계·거리·필터·record·사용자 값·항상 보임 bit)과 page 머리다.
    usize pageBytesUsed = 0;
    usize pageBytesReserved = 0;
    usize tileMapBytesReserved = 0;
    usize listBytesUsed = 0;
    usize listBytesReserved = 0;
    // ---- 누적 계수 ----
    // 같은 타일에서 그 자리만 다시 쓴 경계 갱신 수(목록 안 갱신 포함)다.
    uint64 inPlaceUpdates = 0;
    // 다른 타일 또는 목록과 page 사이로 옮긴 경계 갱신 수다.
    uint64 relocations = 0;
    // 목록에 넣은 횟수(추가·갱신)다.
    uint64 listInsertions = 0;
    uint64 pageAllocations = 0;
    uint64 pageReleases = 0;
};

// FlatBoundsValidationReport::invariant 값이다.
inline constexpr uint32 FlatInvariantNone = 0;
// 레코드와 저장 위치(page·목록)가 서로 가리키지 않는다.
inline constexpr uint32 FlatInvariantRecordLink = 1;
// 타일·page 구조(타일 대응표, page 연결, 가득 찬 page가 앞, 사용 수, 빈 page 없음)가 어긋났다.
inline constexpr uint32 FlatInvariantPageStructure = 2;
// 저장 float 경계가 정확한 상대 경계를 포함하지 않거나 R을 넘거나, 원소가 중심 타일에 있지 않다.
inline constexpr uint32 FlatInvariantStoredBounds = 3;
// 목록 원소가 목록 기준(큰 원소 또는 범위 밖)에 맞지 않거나 page 원소가 목록 기준에 맞다.
inline constexpr uint32 FlatInvariantListPlacement = 4;
// page 속성 배열(필터·거리·사용자 값·항상 보임 bit·거리 제한 수)이 레코드와 다르다.
inline constexpr uint32 FlatInvariantAttributes = 5;
// 전체 원소 수가 레코드 수와 다르다.
inline constexpr uint32 FlatInvariantCount = 6;

// 위반이 page에 속하지 않을 때의 FlatBoundsValidationReport::page 값이다.
inline constexpr uint32 FlatBoundsNoPage = NumericLimits<uint32>::Max();

// FlatBoundsReader::Validate의 결과다(DebugAllocator::Validate와 같은 관례: Result는 검사 호출이
// 수행되었는지, 실제 손상 여부는 이 값이다). 첫 위반 하나만 담는다.
struct FlatBoundsValidationReport {
    uint32 invariant = FlatInvariantNone;
    // 위반을 발견한 page 색인이다. page에 속하지 않으면 FlatBoundsNoPage다.
    uint32 page = FlatBoundsNoPage;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return invariant == FlatInvariantNone;
    }
};

// 원소 하나의 저장 위치다(진단·시험용, FlatBoundsReader::TryPlacementOf).
struct FlatBoundsPlacement {
    uint32 slot = 0;
    // 큰 원소·범위 밖 목록에 있다. 거짓이면 page에 있다.
    bool inList = false;
    // 목록 원소이면 큰 원소(참)인지 범위 밖(거짓)인지다.
    bool large = false;
    // page 원소이면 타일 원점과 page 반경 R이다.
    Vec3d tileOrigin = Vec3d(0.0, 0.0, 0.0);
    float pageRadius = 0.0f;
};

} // namespace rocks::spatial
