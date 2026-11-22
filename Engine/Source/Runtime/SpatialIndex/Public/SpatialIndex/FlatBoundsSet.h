#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/IndexPool.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/AccessOverlapChecker.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/PlatformMath.h>
#include <SpatialIndex/FlatBoundsTypes.h>
#include <SpatialIndex/LooseOctreeTypes.h>

namespace rocks {
class WorkerScheduler;
} // namespace rocks

namespace rocks::spatial {

class FlatBoundsSet;
class FlatBoundsReader;
class FlatBoundsReadScope;

namespace detail {

// 저장 구조(Docs/Design/FlatBounds.ko.md 5절)다. FlatBoundsSet 내부 표현이며 공개 API가 아니다.
// 템플릿 순회(ForEachVisible)가 헤더에서 읽으므로 여기에 둔다.

inline constexpr uint32 FlatNoPage = NumericLimits<uint32>::Max();

// 읽기·변경 단계 위반의 Fatal 메시지다(8절).
inline constexpr AccessOverlapMessages FlatBoundsAccessMessages{
    .writeDuringRead = "FlatBoundsSet mutation while a read scope is open",
    .readDuringWrite = "FlatBoundsSet read scope opened during a mutation",
    .writeDuringWrite = "FlatBoundsSet mutation overlaps another mutation",
    .destroyedWhileOpen = "FlatBoundsSet destroyed while a read scope is open",
    .unbalancedRelease = "FlatBoundsSet access released without a matching acquire"};

// 타일 key(축별 floor((중심 − anchor) / tileSize))다.
struct FlatTileKey {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;

    friend constexpr bool operator==(const FlatTileKey& left, const FlatTileKey& right) noexcept {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
};

struct FlatTileKeyHash {
    uint64 operator()(const FlatTileKey& key) const noexcept {
        return HashCombine(
            HashCombine(MixHash(static_cast<uint32>(key.x)), static_cast<uint32>(key.y)),
            static_cast<uint32>(key.z));
    }
};

// 타일 하나의 page 목록이다. page는 연결 목록이고 마지막 page만 덜 찰 수 있다.
struct FlatTile {
    uint32 firstPage = FlatNoPage;
    uint32 lastPage = FlatNoPage;
    uint32 pageCount = 0;
};

// page 머리다(IndexPool 원소). 원소 배열은 FlatBoundsSet의 page SoA 배열에서 page 색인 ×
// pageCapacity부터다.
struct FlatPageHeader {
    FlatTileKey key;
    // 타일 원점(key × tileSize + anchor, double로 정확하다)이다.
    Vec3d origin = Vec3d(0.0, 0.0, 0.0);
    uint32 count = 0;
    // 저장된 float 상대 좌표의 최대 절댓값 R이다(6.2절). 원소가 들어올 때 늘리고 page가 빌 때만
    // 0으로 돌린다(크게 남는 것은 보수적이라 허용한다).
    float radius = 0.0f;
    uint32 next = FlatNoPage;
    uint32 previous = FlatNoPage;
    // 그리기 거리 하한이나 상한이 있는 원소 수다. 0이면 거리 판정을 건너뛴다.
    uint32 distanceLimited = 0;
};

// 원소의 관리용 레코드다(SlotArray 원소). 정확한 double AABB와 저장 위치를 가진다. 목록에 있으면
// page는 FlatNoPage이고 position은 목록 색인이다.
struct FlatRecord {
    Box<double> bounds;
    uint32 page = FlatNoPage;
    uint32 position = 0;
    uint32 filter = 0;
    float minDraw = 0.0f;
    float maxDraw = 0.0f;
    bool alwaysVisible = false;
    // 목록 원소이면 큰 원소인지(거짓이면 범위 밖)다.
    bool large = false;
    uint64 userValue = 0;
};

// 큰 원소·범위 밖 목록의 원소다. 질의마다 정확한 double로 판정한다.
struct FlatListEntry {
    Box<double> bounds;
    uint32 filter = 0;
    uint32 record = 0;
    float minDraw = 0.0f;
    float maxDraw = 0.0f;
    bool alwaysVisible = false;
    bool large = false;
    uint64 userValue = 0;
};

// page 하나에 옮긴 평면 하나다(6.2절). float 판정은 n_f·x + w′_f < −δ이면 그 평면에서 바깥이다.
// relation은 page 반경 R로 본 page 전체와의 관계다: AllOutside이면 page의 모든 원소가 정답 술어로도
// 바깥이고, AllInside이면 float 판정이 이 평면에서 어떤 원소도 버리지 않는다(판정 생략과 결과가
// 같다).
struct FlatPagePlane {
    enum class Relation : uint8 {
        Mixed,
        AllInside,
        AllOutside
    };

    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    // w′_f = float(n·o − d)다.
    float w = 0.0f;
    // δ를 float로 위쪽 반올림한 값이다.
    float delta = 0.0f;
    Relation relation = Relation::Mixed;
};

// 평면을 타일 원점 origin, page 반경 radius의 page로 옮기고 δ를 계산한다(6.2절 식). 시험이 초과
// 띠를 판정할 때도 이 함수의 δ를 쓴다.
ROCKS_SPATIALINDEX_API FlatPagePlane PreparePagePlane(const Plane<double>& plane,
                                                      const Vec3d& origin, float radius) noexcept;

} // namespace detail

// 매 프레임 뷰 가시성 판정용 평면 경계 집합이다(Docs/Design/FlatBounds.ko.md). 트리 없이 원소
// 경계를 204,800 cm 타일별 조밀한 SoA page에 타일 상대 float로 두고, 뷰마다 page를 전수 판정해 원소
// 슬롯당 1 bit인 가시성 비트 집합을 만든다(UE 5.8 기본 프러스텀 컬링 경로의 구조).
//
// 보수성(6.2절): 정답 술어(6.1절 — 필터, 항상 보임, Core Overlap(volume, box), AABB 거리)로 보이는
// 원소는 반드시 보인다. float 판정은 평면마다 오차 여유 δ만큼 넓히므로 초과(δ 띠 안의 원소)는 있을
// 수 있다. 정확한(실수) 교차 집합도 포함한다. 큰 원소·범위 밖 목록은 double로 정답 술어와 같게
// 판정한다.
//
// 소유와 수명
// - 소비자(RenderScene 등)가 인스턴스를 소유한다. Allocator를 빌리며 allocator는 집합보다 오래
//   살아야 한다.
// - 복사할 수 없고 이동할 수 있다. 이동 뒤 기존 handle은 대상에서 유효하다. 이동된 원본은 빈
// 상태다.
// - 읽기 범위(FlatBoundsReadScope)가 열려 있는 동안 이동·파괴하면 platform::Fatal이다.
//
// 동시성(8절): LooseOctree와 같다. 변경 단계는 한 스레드만 변경 함수를 부르고, 읽기 단계는
// FlatBoundsReadScope로 얻은 FlatBoundsReader로 여러 스레드가 동시에 질의해도 된다. 두 단계가
// 겹치면 platform::Fatal이다.
//
// 슬롯(4절): page 원소의 슬롯은 page 색인 × pageCapacity + page 안 위치이고, 목록 원소는
// FlatBoundsListSlotBase + 목록 색인이다. 삭제·타일 이동·목록 이동은 슬롯을 바꾸며 변경 함수가 그
// 호출로 슬롯이 정해지거나 바뀐 원소를 FlatBoundsMoves로 돌려준다(슬롯 변경 콜백은 없다).
//
// 실패 계약: 빈 handle은 InvalidArgument, 낡은 handle은 StaleHandle, 유효하지 않은 AABB·그리기
// 거리(NaN·음수·Inf·min > max)는 InvalidArgument, 원소 수 한도 초과는 CapacityExceeded, page 색인
// 폭 초과는 Overflow, 할당 실패는 OutOfMemory다. 실패한 변경 호출은 논리 상태(원소 집합, 각 원소의
// 내용·handle·슬롯)를 바꾸지 않는다.
//
// 사용 예:
//   FlatBoundsConfig config;   // 기본값: 204,800 cm 타일, page 64
//   Result<FlatBoundsSet> created = FlatBoundsSet::TryCreate(config, allocator);
//   if (!created) {
//       return Result<void>::Failure(created.Error());
//   }
//   FlatBoundsSet set = Move(created.Value());
//
//   FlatBoundsDesc desc;
//   desc.bounds = primitiveBounds;
//   desc.filterMask = FilterOpaque;
//   desc.maxDrawDistance = 50000.0f;
//   desc.userValue = primitiveIndex;
//   FlatBoundsMoves moves;
//   Result<FlatBoundsHandle> added = set.TryAdd(desc, &moves);     // 변경 단계
//   ApplyMoves(moves);                                             // 소비자 슬롯 배열 갱신
//   ...
//   {
//       FlatBoundsReadScope read(set);                             // 읽기 단계
//       FlatBoundsReader reader = read.Reader();
//       words.TryResize(reader.WordCount());
//       FlatVisibility out{Span<uint64>(words.Data(), words.Size()), 0};
//       FlatBoundsView view{frustum, cameraOrigin, 1.0f, FilterOpaque};
//       auto queried = reader.QueryViews(Span<const FlatBoundsView>(&view, 1),
//                                        Span<FlatVisibility>(&out, 1), &scheduler, scratch);
//       auto visited = reader.ForEachVisible(out, [&](const FlatVisibleElement& element) noexcept {
//           Submit(element.userValue);
//           return QueryControl::Continue;
//       });
//   }
class ROCKS_SPATIALINDEX_API FlatBoundsSet {
public:
    // 설정을 검사하고 빈 집합을 만든다. 저장소는 첫 변경에서 잡는다. 오류(InvalidArgument):
    // FlatBoundsConfig의 범위 위반.
    [[nodiscard]] static Result<FlatBoundsSet> TryCreate(const FlatBoundsConfig& config,
                                                         Allocator& allocator) noexcept;

    FlatBoundsSet(FlatBoundsSet&& other) noexcept;
    FlatBoundsSet& operator=(FlatBoundsSet&& other) noexcept;
    FlatBoundsSet(const FlatBoundsSet&) = delete;
    FlatBoundsSet& operator=(const FlatBoundsSet&) = delete;
    ~FlatBoundsSet() noexcept;

    // ---- 변경 단계 전용 ----

    // 원소를 등록하고 handle을 돌려준다. 중심 타일의 마지막 page 끝(자리가 없으면 새 page)에 두고,
    // 타일 key가 int32 범위 밖이면 범위 밖 목록, 타일 상대 좌표의 최대 절댓값이
    // largeRelativeExtent를 넘으면 큰 원소 목록에 둔다. moves가 있으면 새 원소의 슬롯을 담는다(다른
    // 원소는 옮기지 않는다). 오류: desc가 유효하지 않으면 InvalidArgument, 원소 수가 maxElements에
    // 닿았거나 handle slot이 모두 폐기되었으면 CapacityExceeded, page 색인 폭 초과는 Overflow, 할당
    // 실패는 OutOfMemory. 실패하면 논리 상태는 그대로다.
    [[nodiscard]] Result<FlatBoundsHandle> TryAdd(const FlatBoundsDesc& desc,
                                                  FlatBoundsMoves* moves = nullptr) noexcept;

    // 원소를 지운다. 그 타일 마지막 page의 마지막 원소(목록이면 목록 마지막 원소)가 빈자리로 오며
    // moves가 있으면 그 원소의 새 슬롯을 담는다. 할당하지 않는다. 오류: 빈 handle은
    // InvalidArgument, 낡은 handle은 StaleHandle.
    [[nodiscard]] Result<void> TryRemove(FlatBoundsHandle handle,
                                         FlatBoundsMoves* moves = nullptr) noexcept;

    // 원소 AABB를 바꾼다. 같은 타일이고 목록 여부가 같으면 그 자리만 다시 쓰고(슬롯 그대로), 그
    // 밖에는 새 자리를 먼저 확보한 뒤 옮긴다. moves에는 슬롯이 바뀐 원소(자기 자신과 빈자리를 채운
    // 원소)를 담는다. 오류: handle 오류, bounds가 유효하지 않으면 InvalidArgument, 할당 실패는
    // OutOfMemory(원소는 옛 AABB로 옛 자리에 남는다).
    [[nodiscard]] Result<void> TryUpdateBounds(FlatBoundsHandle handle, const Box<double>& bounds,
                                               FlatBoundsMoves* moves = nullptr) noexcept;

    // 필터 mask를 바꾼다(0이면 어떤 뷰에도 보이지 않는다). 할당하지 않는다. 오류: handle 오류.
    [[nodiscard]] Result<void> TrySetFilter(FlatBoundsHandle handle, uint32 filterMask) noexcept;

    // 그리기 거리 하한·상한(cm, 0은 제한 없음)을 바꾼다. 할당하지 않는다. 오류: handle 오류, 값이
    // NaN·음수·Inf이거나 상한이 0이 아니면서 하한보다 작으면 InvalidArgument.
    [[nodiscard]] Result<void> TrySetDrawDistance(FlatBoundsHandle handle, float minDistance,
                                                  float maxDistance) noexcept;

    // 항상 보임 여부를 바꾼다. 할당하지 않는다. 오류: handle 오류.
    [[nodiscard]] Result<void> TrySetAlwaysVisible(FlatBoundsHandle handle,
                                                   bool alwaysVisible) noexcept;

    // 사용자 값을 바꾼다. 할당하지 않는다. 오류: handle 오류.
    [[nodiscard]] Result<void> TrySetUserValue(FlatBoundsHandle handle, uint64 userValue) noexcept;

    // 원소 count개의 레코드 저장소와 그만큼의 page 원소 저장소를 미리 잡는다(대량 생성 전). 실제
    // page 수는 타일 분포에 따라 더 필요할 수 있다. 오류: count가 maxElements를 넘으면
    // CapacityExceeded, page 색인 폭 초과는 Overflow, 할당 실패는 OutOfMemory(논리 상태는 그대로).
    [[nodiscard]] Result<void> TryReserve(uint32 count) noexcept;

    // 모든 원소를 지운다. handle 세대 번호는 유지되므로 지우기 전 handle은 StaleHandle이 된다. 예약
    // 용량은 유지한다. 누적 통계는 지우지 않는다.
    void Clear() noexcept;

private:
    friend class FlatBoundsReader;
    friend class FlatBoundsReadScope;

    class WriterGuard;

    // 원소가 놓일 곳이다(삽입·갱신이 계산한다).
    struct Target {
        bool list = false;
        bool large = false;
        detail::FlatTileKey key;
        Vec3d origin = Vec3d(0.0, 0.0, 0.0);
    };

    FlatBoundsSet(const FlatBoundsConfig& config, Allocator& allocator) noexcept;

    void TakeFrom(FlatBoundsSet& other) noexcept;
    void CheckNoOpenScope(const char* message) const noexcept;
    Result<void> CheckHandle(FlatBoundsHandle handle) const noexcept;

    // ---- 배치(5절) ----
    Target TargetOf(const Box<double>& bounds) const noexcept;
    // target에 원소 하나를 더 둘 저장소(타일 등록, page, page 배열, 목록)를 확보한다. 실패하면 일부
    // 용량만 늘어 있을 수 있고 논리 상태는 그대로다.
    Result<void> ReserveFor(const Target& target) noexcept;
    // 확보한 저장소에 원소를 붙이고 슬롯을 돌려준다. 할당하지 않는다.
    uint32 Place(uint32 record, const Target& target) noexcept;
    // 원소를 자리에서 빼고 빈자리를 채운다. 빈자리로 옮긴 원소가 있으면 그 레코드 색인을, 없으면
    // NumericLimits<uint32>::Max()를 돌려준다.
    uint32 Unplace(uint32 record) noexcept;
    void WritePageElement(uint32 page, uint32 position, uint32 record) noexcept;
    void WritePageBounds(uint32 page, uint32 position, const Box<double>& bounds) noexcept;
    void CopyPageElement(uint32 fromSlot, uint32 toSlot) noexcept;
    void SetAlwaysBit(uint32 slot, bool value) noexcept;
    void FreePage(uint32 page) noexcept;
    uint32 SlotOfRecord(uint32 record) const noexcept;
    void NoteMove(FlatBoundsMoves* moves, uint32 record) const noexcept;
    Result<void> EnsurePageArrays(uint32 pageSlots) noexcept;

    // ---- 읽기(FlatBoundsReader가 부른다) ----
    uint32 PageSlotCount() const noexcept;
    uint32 WordCount() const noexcept;
    Result<void> QueryViews(Span<const FlatBoundsView> views, Span<FlatVisibility> out,
                            WorkerScheduler* scheduler, Allocator& scratch, uint32 maxHelpers,
                            FlatQueryStats* stats) const noexcept;
    FlatBoundsStats CollectStats() const noexcept;
    Result<FlatBoundsValidationReport> ValidateStructure() const noexcept;
    Result<FlatBoundsDesc> TryGetElement(FlatBoundsHandle handle) const noexcept;
    Result<FlatBoundsPlacement> TryPlacement(FlatBoundsHandle handle) const noexcept;
    Result<uint64> TryUserValueAtSlot(uint32 slot) const noexcept;

    struct Counters {
        uint64 inPlaceUpdates = 0;
        uint64 relocations = 0;
        uint64 listInsertions = 0;
        uint64 pageAllocations = 0;
        uint64 pageReleases = 0;
    };

    FlatBoundsConfig config_;
    Allocator* allocator_ = nullptr;
    uint32 wordsPerPage_ = 1;

    SlotArray<detail::FlatRecord> records_;
    HashMap<detail::FlatTileKey, detail::FlatTile, detail::FlatTileKeyHash> tiles_;
    IndexPool<detail::FlatPageHeader> pages_;
    // page SoA 배열이다. 크기는 pages_.SlotCount() × pageCapacity(항상 보임 bit는 word 단위로
    // / FlatVisibilityBitsPerWord)이고 해제된
    // page의 칸은 내용이 남아 있어도 읽지 않는다(질의는 해제 page의 word를 0으로 쓴다).
    Array<float> minX_;
    Array<float> minY_;
    Array<float> minZ_;
    Array<float> maxX_;
    Array<float> maxY_;
    Array<float> maxZ_;
    // 그리기 거리 하한·상한(cm)이다. 상한이 없으면 +Inf다.
    Array<float> drawMin_;
    Array<float> drawMax_;
    Array<uint32> filter_;
    Array<uint32> record_;
    Array<uint64> userValue_;
    Array<uint64> always_;
    Array<detail::FlatListEntry> list_;
    uint32 largeCount_ = 0;
    Counters counters_;

    // 결과 세대다. 생성·이동·대입·변경 함수 시작마다 프로세스 전역 단조 계수에서 새 값을 받는다
    // (NextMutationEpoch). 이전 상태의 비트 집합을 해석하지 않게 한다.
    uint64 epoch_ = 0;

    AccessOverlapChecker access_{AccessRecursion::ReadsOnly, detail::FlatBoundsAccessMessages};
};

// 읽기 단계의 질의 창구다. FlatBoundsReadScope에서만 얻으며 그 범위가 살아 있는 동안만 쓴다. 복사해
// 여러 스레드에 넘겨도 된다.
class ROCKS_SPATIALINDEX_API FlatBoundsReader {
public:
    FlatBoundsReader(const FlatBoundsReader&) noexcept = default;
    FlatBoundsReader& operator=(const FlatBoundsReader&) noexcept = default;

    // 비트 집합이 다루는 슬롯 수(page 영역 + 목록 원소 수)다.
    [[nodiscard]] uint32 SlotCapacity() const noexcept;

    // 비트 집합 하나에 필요한 word 수다. page 영역은 page마다 pageCapacity /
    // FlatVisibilityBitsPerWord word이고 목록 영역은 그 뒤의 ⌈목록 수 / FlatVisibilityBitsPerWord⌉
    // word다.
    [[nodiscard]] uint32 WordCount() const noexcept;

    // 뷰마다 보이는 원소의 비트 집합을 out에 쓴다(6.3절). out[i]는 views[i]의 결과이며 words의 앞
    // WordCount()개 word를 모두 쓴다: 보이는 원소의 bit만 1이고 해제된 page의 word, page 사용 수
    // 뒤의 꼬리, 목록 영역 마지막 word의 남는 bit는 0이다. epoch에 결과 세대를 쓴다.
    // - scheduler가 nullptr이거나 worker 안에서 부르거나 helper 할당이 실패하면 호출 스레드가 전부
    //   처리한다(ParallelFor 계약). 결과는 병렬 여부와 무관하게 같다. maxHelpers는 helper 수
    //   상한이다(0이면 단독).
    // - 질의가 쓰는 할당은 scratch뿐이며(helper 작업 본문), scratch는 여러 스레드의 해제를 받을 수
    //   있어야 한다.
    // - stats가 있으면 계수를 더한다.
    // 오류(InvalidArgument, 출력 불변): 유효하지 않은 volume, 비유한 distanceOrigin,
    // distanceScale이 유한 양수가 아님, views와 out 개수 불일치, out[i].words가 WordCount()보다
    // 짧음, out 버퍼끼리 겹침. 뷰 0개는 성공(no-op).
    [[nodiscard]] Result<void>
    QueryViews(Span<const FlatBoundsView> views, Span<FlatVisibility> out,
               WorkerScheduler* scheduler, Allocator& scratch, FlatQueryStats* stats = nullptr,
               uint32 maxHelpers = NumericLimits<uint32>::Max()) const noexcept {
        return set_->QueryViews(views, out, scheduler, scratch, maxHelpers, stats);
    }

    // 비트 집합의 보이는 원소마다 visitor(const FlatVisibleElement&)를 슬롯 순서(page 영역, 목록
    // 영역)로 부른다. visitor는 QueryControl을 돌려주며 Stop이면 멈춘다. 오류: bits가 이 집합의
    // 지금 상태로 만든 결과가 아니면(결과 세대가 다름) StaleHandle, words가 WordCount()보다 짧으면
    // InvalidArgument.
    template <typename Visitor>
    [[nodiscard]] Result<void> ForEachVisible(const FlatVisibility& bits,
                                              Visitor&& visitor) const noexcept;

    // 슬롯의 사용자 값이다. 오류: 살아 있지 않거나 범위 밖 슬롯은 NotFound.
    [[nodiscard]] Result<uint64> TryUserValueAt(uint32 slot) const noexcept {
        return set_->TryUserValueAtSlot(slot);
    }

    // 원소의 지금 슬롯이다. 오류: 빈 handle은 InvalidArgument, 낡은 handle은 StaleHandle.
    [[nodiscard]] Result<uint32> TrySlotOf(FlatBoundsHandle handle) const noexcept;

    // 원소의 저장 위치(슬롯, 목록 여부, 타일 원점, page 반경)다. 진단·시험용이다. 오류는
    // TrySlotOf와 같다.
    [[nodiscard]] Result<FlatBoundsPlacement>
    TryPlacementOf(FlatBoundsHandle handle) const noexcept {
        return set_->TryPlacement(handle);
    }

    [[nodiscard]] bool IsValid(FlatBoundsHandle handle) const noexcept;

    // 원소의 지금 내용이다. 오류: 빈 handle은 InvalidArgument, 낡은 handle은 StaleHandle.
    [[nodiscard]] Result<FlatBoundsDesc> TryGet(FlatBoundsHandle handle) const noexcept {
        return set_->TryGetElement(handle);
    }

    // 살아 있는 원소 수(목록 포함)다.
    [[nodiscard]] uint32 Count() const noexcept;

    // 구조·메모리·누적 계수다. O(page 수)다.
    [[nodiscard]] FlatBoundsStats Stats() const noexcept {
        return set_->CollectStats();
    }

    // 저장 구조의 일관성을 모두 검사하고 첫 위반을 report에 담는다(DebugAllocator::Validate 관례).
    // 할당하지 않으므로 호출 자체는 실패하지 않는다. O(원소 수)이므로 시험과 진단에서만 부른다.
    [[nodiscard]] Result<FlatBoundsValidationReport> Validate() const noexcept {
        return set_->ValidateStructure();
    }

private:
    friend class FlatBoundsReadScope;

    explicit FlatBoundsReader(const FlatBoundsSet& set) noexcept : set_(&set) {
    }

    const FlatBoundsSet* set_;
};

// 읽기 단계 하나를 여는 범위다(8절). 집합의 접근 검출기(Core AccessOverlapChecker)에 읽기 범위를
// 연다: 변경 중이면 platform::Fatal이고, 범위가 살아 있는 동안 집합을 변경하면 그 변경 또는 이
// 범위의 종료가 Fatal이다. 같은 스레드에서 겹쳐 열어도 된다. QueryViews의 worker는 범위를 따로 열지
// 않는다 (호출 스레드의 범위가 질의 전체를 덮는다). 범위는 복사·이동할 수 없고 만든 스레드에서
// 끝낸다.
class FlatBoundsReadScope {
public:
    explicit FlatBoundsReadScope(const FlatBoundsSet& set) noexcept
        : set_(&set), access_(set.access_) {
    }

    ~FlatBoundsReadScope() noexcept = default;

    FlatBoundsReadScope(const FlatBoundsReadScope&) = delete;
    FlatBoundsReadScope& operator=(const FlatBoundsReadScope&) = delete;
    FlatBoundsReadScope(FlatBoundsReadScope&&) = delete;
    FlatBoundsReadScope& operator=(FlatBoundsReadScope&&) = delete;

    [[nodiscard]] FlatBoundsReader Reader() const noexcept {
        return FlatBoundsReader(*set_);
    }

private:
    const FlatBoundsSet* set_;
    ReadAccessScope access_;
};

// ---- 템플릿 구현 ----

template <typename Visitor>
Result<void> FlatBoundsReader::ForEachVisible(const FlatVisibility& bits,
                                              Visitor&& visitor) const noexcept {
    const FlatBoundsSet& set = *set_;
    if (bits.epoch != set.epoch_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    const uint32 wordCount = set.WordCount();
    if (bits.words.Size() < wordCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const uint32 pageWords = set.PageSlotCount() * set.wordsPerPage_;
    for (uint32 wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
        uint64 word = bits.words[wordIndex];
        while (word != 0) {
            const uint32 bit = platform::PlatformMath::CountTrailingZeros64(word);
            word &= word - 1;
            FlatVisibleElement element;
            uint32 record = 0;
            if (wordIndex < pageWords) {
                element.slot = wordIndex * FlatVisibilityBitsPerWord + bit;
                record = set.record_[element.slot];
                element.userValue = set.userValue_[element.slot];
            } else {
                const uint32 index = (wordIndex - pageWords) * FlatVisibilityBitsPerWord + bit;
                const detail::FlatListEntry& entry = set.list_[index];
                element.slot = FlatBoundsListSlotBase + index;
                record = entry.record;
                element.userValue = entry.userValue;
            }
            element.handle = FlatBoundsHandle{set.records_.HandleAt(record)};
            if (visitor(static_cast<const FlatVisibleElement&>(element)) == QueryControl::Stop) {
                return Result<void>::Success();
            }
        }
    }
    return Result<void>::Success();
}

} // namespace rocks::spatial
