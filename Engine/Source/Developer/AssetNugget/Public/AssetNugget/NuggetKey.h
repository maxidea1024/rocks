#pragma once

#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Object/AssetId.h>

// 에셋 너겟 키다(Docs/Design/AssetSystem.ko.md 9절·9.1절 결정 2–4). 처리기는 결과에 영향을 주는
// 입력을 고정 순서로 NuggetKeyWriter에 쓰고 그 SHA-256이 키다. 모든 기록은 태그(u8) + 값이고 가변
// 길이에는 길이 접두가 있어 이웃 기록과 경계가 모호하지 않다.
//
// 사용 예(처리기 키):
//   NuggetKeyWriter writer;
//   WriteNuggetKeyHeader(writer, TextureProcessor, projectGeneration);
//   writer.WriteList(0);                       // 인코더 목록(비압축 — 없음)
//   writer.WriteGuid(typeId.value);
//   writer.WriteDigest(sourceBulkSha256);
//   auto settings = WriteReflected(writer, settingsTarget);   // NuggetReflected.h
//   auto key = writer.Finish();                // Result<Sha256Digest>
namespace rocks {

// 기록 태그다(9.1절 결정 2 — 값은 고정된다, 바꾸면 키 형식 버전을 올린다).
enum class NuggetKeyTag : uint8 {
    None = 1,
    Bool,
    U32,
    U64,
    I64,
    F32,
    F64,
    Guid,
    Digest,
    String,
    Bytes,
    List,
    Map,
    Set,
    Field,
    Struct,
    End
};

// 키 기록을 사람이 읽는 줄로 받는 선택 sink다(미적중 원인 추적·golden — 9.1절 결정 2). 줄은 이
// 호출 동안만 유효하다.
class NuggetKeyTrace {
public:
    virtual ~NuggetKeyTrace() = default;
    virtual void Line(StringView text) noexcept = 0;
};

// 키 기록기다. 기본은 SHA-256에 흘려 넣고(byte를 모으지 않는다), buffer를 주면 그 배열 끝에
// 인코딩 byte를 붙인다(Map·Set 항목 정렬용 — Raw로 다시 흘려 넣는다).
//
// 오류는 기록기 상태다: 유효하지 않은 UTF-8 String·Field는 InvalidArgument, buffer 할당 실패는
// OutOfMemory로 첫 오류를 남기고 이후 기록을 무시한다. Finish·Error가 그 오류를 돌려준다.
// 한 스레드에서 쓴다.
class ROCKS_ASSETNUGGET_API NuggetKeyWriter {
public:
    NuggetKeyWriter() noexcept = default;
    // 인코딩 byte를 buffer 끝에 붙이는 기록기다(해시하지 않는다 — Finish는 InvalidArgument).
    explicit NuggetKeyWriter(Array<uint8>& buffer) noexcept : buffer_(&buffer) {
    }

    NuggetKeyWriter(const NuggetKeyWriter&) = delete;
    NuggetKeyWriter& operator=(const NuggetKeyWriter&) = delete;

    // 기록 줄을 받을 sink다(nullptr이면 없음 — 비용이 없다). sink는 기록기보다 오래 살아야 한다.
    void SetTrace(NuggetKeyTrace* trace) noexcept {
        trace_ = trace;
    }

    void WriteNone() noexcept;
    void WriteBool(bool value) noexcept;
    void WriteU32(uint32 value) noexcept;
    void WriteU64(uint64 value) noexcept;
    void WriteI64(int64 value) noexcept;
    // 실수는 bit 패턴이다. NaN은 부호·payload와 무관하게 quiet NaN 하나로 단일화하고 -0과 +0은
    // 다르다.
    void WriteF32(float value) noexcept;
    void WriteF64(double value) noexcept;
    void WriteGuid(Guid value) noexcept;
    void WriteDigest(const Sha256Digest& value) noexcept;
    // u64 길이 + UTF-8. 유효하지 않은 UTF-8은 InvalidArgument 상태가 된다.
    void WriteString(StringView value) noexcept;
    void WriteBytes(Span<const uint8> value) noexcept;
    // 원소 수다(원소는 이어지는 기록). Map 항목·Set 원소는 정규 인코딩 byte 사전순으로 쓴다(호출자
    // 책임 — WriteReflected가 정렬한다).
    void WriteList(uint64 count) noexcept;
    void WriteMap(uint64 count) noexcept;
    void WriteSet(uint64 count) noexcept;
    // 필드 이름이다(리플렉션 기록 — 뒤에 그 값). 유효하지 않은 UTF-8은 InvalidArgument 상태.
    void WriteField(StringView name) noexcept;
    // struct 시작·끝이다.
    void WriteStructBegin() noexcept;
    void WriteStructEnd() noexcept;
    // 이미 인코딩한 기록 byte를 그대로 흘려 넣는다(태그를 붙이지 않는다 — 정렬한 Map·Set 항목).
    void WriteRaw(Span<const uint8> encoded) noexcept;

    // 오류 상태로 만든다(이미 오류면 첫 오류를 둔다 — 키 기록을 돕는 함수가 자기 실패를 기록기에
    // 남길 때 쓴다). ErrorCode::None은 무시한다.
    void Fail(ErrorCode error) noexcept {
        if (error_ == ErrorCode::None && error != ErrorCode::None) {
            error_ = error;
        }
    }

    // 첫 오류다(없으면 성공).
    Result<void> Error() const noexcept {
        return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
    }

    // 키(SHA-256)를 돌려준다. 오류 상태면 그 오류, buffer 기록기면 InvalidArgument다. 한 번만
    // 부른다(다시 부르면 InvalidArgument).
    Result<Sha256Digest> Finish() noexcept;

private:
    void Emit(Span<const uint8> bytes) noexcept;
    void Tag(NuggetKeyTag tag) noexcept;
    void Note(StringView text) noexcept;

    Sha256 hasher_;
    Array<uint8>* buffer_ = nullptr;
    NuggetKeyTrace* trace_ = nullptr;
    ErrorCode error_ = ErrorCode::None;
    bool finished_ = false;
};

// 처리기 정체성이다(키 공통 머리 — 처리기 Guid와 처리기 버전 Guid. 출력 의미가 바뀌면 버전 Guid를
// 바꾼다).
struct NuggetProcessorId {
    Guid processor{};
    Guid version{};
};

// 키 형식 버전이다(태그·공통 머리 배치가 바뀌면 올린다).
inline constexpr uint32 NuggetKeyFormatVersion = 1;

// 엔진 세대 Guid다(9.1절 결정 4 — 엔진 결함으로 오염된 항목을 회수하려면 이 값을 바꾼다. 모든 키가
// 바뀐다).
inline constexpr Guid EngineNuggetGeneration{0x524e554747454e31ull, 0x0000000000000001ull};

// 키 공통 머리를 쓴다(9.1절 결정 2): "RocksAssetNugget"·키 형식 버전·엔진 세대·프로젝트 세대
// (없으면 null Guid)·처리기 Guid·처리기 버전 Guid·결정적 수학 버전·툴체인 지문.
ROCKS_ASSETNUGGET_API void WriteNuggetKeyHeader(NuggetKeyWriter& writer,
                                                const NuggetProcessorId& processor,
                                                Guid projectGeneration) noexcept;

// 결과를 만드는 인코더·외부 라이브러리 하나를 쓴다(이름·버전·설정 byte — 처리기가 WriteList(n)
// 뒤에 n번 부른다).
ROCKS_ASSETNUGGET_API void WriteNuggetEncoder(NuggetKeyWriter& writer, StringView name,
                                              StringView version,
                                              Span<const uint8> settings) noexcept;

// SCC 구성원이다(9.1절 결정 3): AssetId와 지역 입력 키(의존을 뺀 입력의 키).
struct NuggetSccMember {
    AssetId asset{};
    Sha256Digest localKey{};
};

// SCC 밖 의존 튜플이다: 구성원 AssetId, 역할(UTF-8), 의존 AssetId, 그 의존의 구성원 key.
struct NuggetSccDependency {
    AssetId member{};
    StringView role{};
    AssetId dependency{};
    Sha256Digest dependencyKey{};
};

// SCC key다(9절·9.1절 결정 3). 구성원·튜플을 정규 인코딩 byte 사전순으로 정렬한다(입력 순서와
// 무관). 빈 구성원 목록·구성원 AssetId 중복·null AssetId·유효하지 않은 UTF-8 역할, 튜플의 구성원이
// 구성원이 아님·의존이 구성원임(SCC 안 간선)은 InvalidArgument, 튜플의 완전 중복은 하나로 합친다.
// 정렬 자리 할당 실패는 OutOfMemory다.
ROCKS_ASSETNUGGET_API Result<Sha256Digest>
NuggetSccKey(Allocator& allocator, Span<const NuggetSccMember> members,
             Span<const NuggetSccDependency> external) noexcept;

// 구성원 key = SHA-256(태그, SCC key, AssetId)다(Merkle 입력·구성원 조회용 — Record는 SCC key로
// 저장한다).
ROCKS_ASSETNUGGET_API Sha256Digest NuggetMemberKey(const Sha256Digest& sccKey,
                                                   AssetId asset) noexcept;

} // namespace rocks
