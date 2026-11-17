#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 셰이더 도구 스파이크(SH0, Docs/Design/ShaderSystem.ko.md 16절)의 공통 형식이다. 스파이크는 단일
// 스레드 측정 프로그램이며 이 파일의 형식은 모두 그 스레드에서만 쓴다.
namespace rocks::spike {

using ByteArray = Array<uint8>;

// 스파이크 전체가 쓰는 할당기다(프로세스 수명의 SystemAllocator).
Allocator& SpikeAllocator() noexcept;

enum class ShaderStage : uint8 {
    Vertex,
    Pixel,
    Compute
};

// 컴파일할 HLSL 입력 하나다. 문자열은 모두 정적 수명이거나 이 값보다 오래 산다.
struct ShaderInput {
    // 산출물 파일 이름 앞부분이다(예: "Renderer.vs"). 경로 구분자를 넣지 않는다.
    const char* name = nullptr;
    // NUL 종료 HLSL 원문.
    const char* source = nullptr;
    const char* entry = nullptr;
    ShaderStage stage = ShaderStage::Vertex;
};

// 셰이더 모델 프로필 앞부분이다("vs"·"ps"·"cs").
const char* StageProfilePrefix(ShaderStage stage) noexcept;

// JSON 보고서를 쓴다(Core JsonWriter 위의 얇은 층). 쓰기 실패(메모리 부족·구조 오류)는 첫 실패를
// 기억해 Finish가 false를 돌려주며, 그 사이의 호출은 아무 일도 하지 않는다. 값 종류마다 이름이 다른
// 것은 문자열 리터럴이 bool 오버로드로 바뀌는 실수를 막기 위해서다.
class Report final {
public:
    Report() noexcept;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;

    void BeginObject() noexcept;
    void BeginObject(StringView key) noexcept;
    void EndObject() noexcept;
    void BeginArray(StringView key) noexcept;
    void EndArray() noexcept;
    void String(StringView key, StringView value) noexcept;
    void Bool(StringView key, bool value) noexcept;
    void UInt(StringView key, uint64 value) noexcept;
    void Double(StringView key, double value) noexcept;
    // 배열 원소 문자열이다(BeginArray 안에서 쓴다).
    void Element(StringView value) noexcept;

    // 최상위 값을 닫고 끝 줄바꿈을 쓴다. 지금까지 실패가 없었으면 true다.
    bool Finish() noexcept;

    // 쓴 JSON이다. 다음 쓰기 호출까지 유효하다.
    StringView Text() const noexcept {
        return text_.View();
    }

private:
    void Remember(Result<void> result) noexcept;

    StringBuilder<4096> text_;
    JsonWriter writer_;
    bool failed_ = false;
};

// 산출물 디렉터리다. 쓴 파일마다 이름·크기·SHA-256을 기억해 manifest.json(실행마다 같아야 하는
// 결정적 내용만, 시각·경로·측정값 없음)으로 남긴다. 결정성 측정 S3는 서로 다른 실행의
// manifest.json을 비교한다.
class OutputDirectory final {
public:
    // root는 UTF-8 경로(NUL 종료)다. 디렉터리는 Create가 만든다. root는 이 객체보다 오래 산다.
    explicit OutputDirectory(const char* root) noexcept;
    OutputDirectory(const OutputDirectory&) = delete;
    OutputDirectory& operator=(const OutputDirectory&) = delete;

    // root와 없는 부모 디렉터리를 만든다. 실패하면 stderr에 원인을 쓰고 false다.
    bool Create() noexcept;

    // root/name에 bytes를 쓰고 manifest에 기록한다. 같은 이름을 다시 쓰면 실패다.
    bool Write(const char* name, Span<const uint8> bytes) noexcept;
    bool WriteText(const char* name, StringView text) noexcept;

    // manifest에 넣지 않는 파일(측정값이 든 report.json)을 쓴다.
    bool WriteUnlisted(const char* name, StringView text) noexcept;

    // 지금까지 쓴 파일 목록을 root/manifest.json으로 쓴다.
    bool WriteManifest() noexcept;

private:
    struct Entry {
        char name[96] = {};
        uint64 bytes = 0;
        Sha256Digest digest;
    };

    bool WriteFile(const char* name, Span<const uint8> bytes) noexcept;

    const char* root_ = nullptr;
    Array<Entry> entries_;
};

// 파일 전체를 bytes에 읽는다(path는 UTF-8 절대 경로). 실패하면 false다.
bool ReadWholeFile(const char* path, ByteArray& bytes) noexcept;

// byte 배열과 문자열 사이의 변환이다.
Span<const uint8> AsBytes(StringView text) noexcept;
StringView AsText(Span<const uint8> bytes) noexcept;

// DXBC·DXIL container 머리의 hash digest다(Docs/Design/ShaderSystem.ko.md 10절 dxil 서명). 머리는
// "DXBC" fourCC(4 byte) 다음 16 byte digest다.
struct ContainerDigest {
    static constexpr usize Bytes = 16;

    uint8 bytes[Bytes] = {};
    // container 머리를 읽었는가(크기·fourCC).
    bool present = false;
    // digest가 0이 아니고 preview 우회 값(0x02 × 16, DXC PreviewByPassHash)도 아니다.
    bool isSigned = false;
};

ContainerDigest ReadContainerDigest(Span<const uint8> container) noexcept;

// digest를 32자 소문자 16진수로 out(33 byte 이상)에 쓴다.
void ContainerDigestHex(const ContainerDigest& digest,
                        char (&out)[ContainerDigest::Bytes * 2 + 1]) noexcept;

} // namespace rocks::spike
