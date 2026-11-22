#pragma once

#include <AssetNugget/NuggetFile.h>
#include <Platform/File.h>

// 파일 시스템 에셋 너겟 backend다(Docs/Design/AssetSystem.ko.md 9절·9.1절 결정 7–9, "A3b 구현
// 세부"). 키 하나를 `.rnug` v2 파일 하나(`<root>/<k0k1>/<key>.rnug`)로 둔다. 쓰기는 임시 파일에 다
// 쓴 뒤 no-replace 이동으로 게시하므로 잠금 없이 여러 프로세스가 같은 루트를 쓴다. 모든 읽기를
// 검증하고 손상 항목은 지운다(틀린 적중 0건 — Rockpile R-0).
//
// Get·Put은 실패를 Result로 돌려주지 않는다 — 캐시 실패는 빌드 실패가 아니므로(R-1) 상태와 원인
// 오류로 돌려준다(`required` 판정은 NuggetCache가 한다).
//
// 사용 예:
//   NuggetSystemClock clock;
//   NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(), root, clock);
//   if (auto opened = backend.Open(); !opened) { ... }
//   const NuggetPutValue values[] = {{0, mip0}, {1, mip1}};
//   NuggetBackendPut put{NuggetBucketTexture, key, values};
//   NuggetPutResult stored = backend.Put(put);           // Stored·Exists·Conflict·Dropped
//   NuggetGetResult hit = backend.Get(NuggetBackendGet{NuggetBucketTexture, key});
//   if (hit.status == NuggetStatus::Hit) { /* hit.values[i].bytes */ }
namespace rocks {

// 요청 결과 상태다(9.1절 결정 13).
enum class NuggetStatus : uint8 {
    Hit,
    Miss,
    Partial,
    Stored,
    Exists,
    Conflict,
    DigestChanged,
    Corrupt,
    Cancelled,
    Unknown,
    Dropped
};

// 시각 공급이다(파일 tick — platform::FileTicksPerSecond 단위, 0001-01-01 UTC 기준).
class NuggetClock {
public:
    virtual ~NuggetClock() = default;
    virtual int64 NowTicks() noexcept = 0;
};

// 벽시계다(DateTime::UtcNow).
class NuggetSystemClock final : public NuggetClock {
public:
    int64 NowTicks() noexcept override;
};

// 읽은 값 하나다(해제한 원본).
struct NuggetValue {
    explicit NuggetValue(Allocator& allocator) noexcept : bytes(allocator) {
    }

    uint32 id = 0;
    Array<uint8> bytes;
};

// Get 요청이다. values가 비면 전체다. partial이면 청한 값 일부만 있어도 Partial, 아니면 Miss다.
// skipData면 값을 읽지 않는다(Record만), skipMeta면 meta를 비운다. touch면 적중 때 사용 시각을
// 갱신한다.
struct NuggetBackendGet {
    StringView bucket{};
    Sha256Digest key{};
    Span<const uint32> values{};
    bool partial = false;
    bool skipData = false;
    bool skipMeta = false;
    bool touch = true;
};

struct NuggetGetResult {
    explicit NuggetGetResult(Allocator& allocator) noexcept : record(allocator), values(allocator) {
    }

    NuggetStatus status = NuggetStatus::Miss;
    // 원인 오류다(Miss의 IO 오류·다른 버전 Unsupported·bucket 불일치 InvalidArgument, Corrupt의
    // 손상 종류, 할당 실패 OutOfMemory). 정상 미적중·적중은 None이다.
    ErrorCode error = ErrorCode::None;
    Sha256Digest digest{};
    NuggetRecord record;
    Array<NuggetValue> values;
};

// Put 값 하나다(원본 byte — backend가 저장 압축을 판단한다). 호출 동안만 유효하면 된다.
struct NuggetPutValue {
    uint32 id = 0;
    Span<const uint8> bytes{};
};

// Put 요청이다(values는 ValueId 오름차순, inputs는 정규 순서 — 정규 형식 위반은 Dropped).
struct NuggetBackendPut {
    StringView bucket{};
    Sha256Digest key{};
    Span<const NuggetPutValue> values{};
    Span<const NuggetInput> inputs{};
    Span<const uint8> meta{};
};

struct NuggetPutResult {
    NuggetStatus status = NuggetStatus::Dropped;
    ErrorCode error = ErrorCode::None;
    // 이 Put의 Record digest와 기존 항목의 digest(Exists·Conflict)다.
    Sha256Digest digest{};
    Sha256Digest existing{};
};

// GC 기준이다(9.1절 결정 9): unusedDays일보다 오래 쓰지 않은 항목을 지우고, 남은 합이 maxBytes를
// 넘으면 오래된 것부터 지운다.
struct NuggetCollectOptions {
    uint64 maxBytes = 0;
    uint32 unusedDays = 0;
};

struct NuggetCollectResult {
    usize removed = 0;
    uint64 removedBytes = 0;
    usize kept = 0;
    uint64 keptBytes = 0;
    // 지우지 못해 건너뛴 항목 수다(열린 파일·권한 등).
    usize skipped = 0;
    // 지운 지난 임시 파일 수다.
    usize temporaries = 0;
};

// 시험 hook의 지점이다(제품 코드는 hook을 걸지 않는다).
enum class NuggetBackendStep : uint8 {
    // Put이 임시 파일을 다 쓰고 이동하기 직전.
    BeforePublish,
    // 이동이 AlreadyExists였고 기존 항목을 판정하기 직전(잠금 밖).
    BeforeJudge,
    // 손상 항목 교체를 위해 shard 잠금을 잡고 다시 판정하기 직전.
    BeforeJudgeLocked
};

// 시험 hook이다: 경합 순서(다른 쓰기·지우기가 끼어드는 자리)를 결정적으로 만든다.
struct NuggetBackendHooks {
    void (*callback)(void* context, NuggetBackendStep step,
                     const Sha256Digest& key) noexcept = nullptr;
    void* context = nullptr;
};

// 저장 압축 임계(원본 byte)·RCBK 블록 크기·임시 파일 수명·shard 잠금 대기 기본값이다(9.1절 결정
// 7·8).
inline constexpr usize NuggetStoreCompressBytes = 64 * 1024;
inline constexpr uint32 NuggetStoreBlockSizeLog2 = 16;
inline constexpr int64 NuggetTemporaryLifetimeTicks = 24ll * 60 * 60 * platform::FileTicksPerSecond;
inline constexpr uint32 NuggetReplaceLockMs = 5000;

class NuggetFileSystemBackend {
public:
    // root는 물리 경로다(shard 잠금 `tmp/<k0k1>.lock` — FileLock). files·clock은 backend보다 오래
    // 살아야 한다.
    NuggetFileSystemBackend(Allocator& allocator, platform::FileSystem& files, StringView root,
                            NuggetClock& clock) noexcept;
    NuggetFileSystemBackend(const NuggetFileSystemBackend&) = delete;
    NuggetFileSystemBackend& operator=(const NuggetFileSystemBackend&) = delete;

    // 루트와 tmp를 만들고(tmp를 만들 수 없으면 읽기 전용 — Get만, Put은 Dropped) 지난 임시 파일을
    // 지운다. 오류: 빈 루트 InvalidArgument, 루트를 만들 수 없음(자리가 파일 등) 그 오류, 할당
    // 실패.
    Result<void> Open() noexcept;

    bool IsReadOnly() const noexcept {
        return readOnly_;
    }

    NuggetGetResult Get(const NuggetBackendGet& request) noexcept;
    NuggetPutResult Put(const NuggetBackendPut& request) noexcept;

    // 사용 시각을 지금으로 갱신한다. 오류: 없음 NotFound, 그 밖 IO 오류.
    Result<void> Touch(const Sha256Digest& key) noexcept;
    // 항목을 지운다(관리·ReportCorrupt). 없었으면 false다. 오류: IO 오류.
    Result<bool> Remove(const Sha256Digest& key) noexcept;

    // GC다(지난 임시 파일 정리 포함). 오류: 루트 열거 실패, 할당 실패.
    Result<NuggetCollectResult> Collect(const NuggetCollectOptions& options) noexcept;

    void SetHooks(const NuggetBackendHooks& hooks) noexcept {
        hooks_ = hooks;
    }

    // 손상 항목 교체의 shard 잠금 대기 상한(ms)이다. 기본 NuggetReplaceLockMs. 넘으면 그 Put은
    // Dropped(Busy)다. 예: 다른 프로세스가 잠금을 쥔 시험에서 짧게.
    void SetReplaceLockTimeout(uint32 milliseconds) noexcept {
        replaceLockMs_ = milliseconds;
    }

private:
    void Notify(NuggetBackendStep step, const Sha256Digest& key) noexcept {
        if (hooks_.callback != nullptr) {
            hooks_.callback(hooks_.context, step, key);
        }
    }

    Result<void> EntryPath(const Sha256Digest& key, String& out) const noexcept;
    Result<void> CleanTemporaries(usize& removed) noexcept;
    NuggetPutResult Settle(const Sha256Digest& key, StringView temporary, StringView entry,
                           NuggetPutResult result) noexcept;

    Allocator* allocator_;
    platform::FileSystem* files_;
    String root_;
    NuggetClock* clock_;
    NuggetBackendHooks hooks_{};
    uint32 replaceLockMs_ = NuggetReplaceLockMs;
    bool opened_ = false;
    bool readOnly_ = false;
};

} // namespace rocks
