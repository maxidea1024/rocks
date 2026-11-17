#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/NameField.h>
#include <Platform/Fatal.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/Test.h>

// 리플렉션 B9c2 Name 필드의 내용 규칙·토큰 검사·FieldValue 변환·wire 시험이다
// (Docs/Design/ReflectionExtensions.ko.md 4절, 17절 B9c 항목): UE 5.8과 같은 길이 규칙(번호 접미를
// 뗀 base 1,023 UTF-16 단위)·UTF-8·NUL·NFC 거부와 검사 순서, pool 없음 Unsupported·다른 pool의 토큰
// WrongContext·게시되지 않은 토큰 StaleHandle·실행 중이 아닌 pool ShuttingDown·구조가 틀린 토큰
// InvalidArgument, None은 빈 표시 문자열, 단일 값 형식 대응과 wire(String), codec의 단계(1단계
// 기록·2단계 토큰 소비 — 2단계는 내용을 다시 보지 않는다)와 관찰 자리 방어. 그래프 왕복·2a
// intern·경로 쓰기는 ObjectSerialization의 NameGraphTests가 생성 fixture로 본다.
using namespace rocks;

namespace {

static_assert(detail::ScalarValueKind<Name>() == ValueKind::Name);
static_assert(detail::FieldValueKind<Name>() == ValueKind::Name);
static_assert(detail::FieldValueKind<Array<Name>>() == ValueKind::Array);
static_assert(ValueKindCanBeArrayElement(ValueKind::Name) &&
              !ValueKindAcceptsRequired(ValueKind::Name));

ErrorCode TextError(StringView text, Allocator& scratch) noexcept {
    auto valid = ValidateNameFieldText(text, scratch);
    return valid ? ErrorCode::None : valid.Error();
}

ErrorCode TokenError(NamePool* pool, Name token, Allocator& scratch) noexcept {
    auto valid = ValidateNameFieldToken(pool, token, scratch);
    return valid ? ErrorCode::None : valid.Error();
}

Name Intern(NamePool& pool, StringView text) noexcept {
    auto interned = pool.TryIntern(text);
    if (!interned) {
        platform::Fatal("test name intern failed");
    }

    return interned.Value();
}

// AcquireNameFieldText가 성공하고 저장할 표시 문자열이 expected인지다.
bool DisplayIs(NamePool* pool, Name token, StringView expected, Allocator& scratch) noexcept {
    auto display = AcquireNameFieldText(pool, token, scratch);
    return display && display.Value().View() == expected;
}

// unit을 count번 이은 문자열이다.
Array<char> Repeat(Allocator& allocator, StringView unit, usize count) noexcept {
    Array<char> text(allocator);
    for (usize index = 0; index < count; ++index) {
        for (usize offset = 0; offset < unit.ByteCount(); ++offset) {
            if (!text.TryPushBack(unit.Data()[offset])) {
                platform::Fatal("test text allocation failed");
            }
        }
    }

    return text;
}

StringView ViewOf(const Array<char>& text) noexcept {
    return StringView(text.Data(), text.Size());
}

// prefix 뒤에 suffix를 이은 문자열이다.
Array<char> Concat(Allocator& allocator, const Array<char>& prefix, StringView suffix) noexcept {
    Array<char> text(allocator);
    for (usize index = 0; index < prefix.Size(); ++index) {
        if (!text.TryPushBack(prefix[index])) {
            platform::Fatal("test text allocation failed");
        }
    }

    for (usize index = 0; index < suffix.ByteCount(); ++index) {
        if (!text.TryPushBack(suffix.Data()[index])) {
            platform::Fatal("test text allocation failed");
        }
    }

    return text;
}

// 모든 할당이 OutOfMemory인 할당기다(시도 수를 센다).
class FailingAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize, usize, uint32 = 0) noexcept override {
        ++attempts;
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    Result<void> Deallocate(void*) noexcept override {
        return Result<void>::Success();
    }

    uint32 attempts = 0;
};

// payload를 그대로 여는 복원 문맥이다. CollectName은 표시 문자열을 기록하고, TakeName은 tokens를
// 기록 순서대로 주며 다시 읽은 표시가 기록과 다르면 InvalidArgument다. Spend는 합만 센다. scratch는
// codec의 임시 할당기(Scratch), storage는 기록 표의 할당기다.
class NameDecodeContext final : public FieldDecodeContext {
public:
    NameDecodeContext(Allocator& scratch, Allocator& storage) noexcept
        : FieldDecodeContext(FieldDecodePolicy{}, nullptr, static_cast<uint32>(WireType::U32)),
          allocator_(scratch), recorded(storage), tokens(storage) {
    }

    void EnterApplyPhase() noexcept {
        SetRecording(false);
    }

    Result<void> CollectName(StringView text) noexcept override {
        return recorded.TryPushBack(text);
    }

    Result<Name> TakeName(StringView text) noexcept override {
        if (taken >= recorded.Size() || !(recorded[taken] == text) || taken >= tokens.Size()) {
            return Result<Name>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Name>::Success(tokens[taken++]);
    }

    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        return ArchiveReader::Create(field.payload);
    }

    Result<RecordScope> OpenRecord(const FieldView&, TypeId, uint32, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenMigratedRecord(Span<const uint8>, TypeId, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<Span<const uint8>> MigrateStruct(TypeId, uint32, uint32,
                                            Span<const uint8>) noexcept override {
        return Result<Span<const uint8>>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordListReader> OpenRecordList(const FieldView&, TypeId) noexcept override {
        return Result<RecordListReader>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenListItem(RecordListReader&, usize) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<DecodedReference> ReadReference(ArchiveReader&,
                                           const ReferenceExpectation&) noexcept override {
        return Result<DecodedReference>::Failure(ErrorCode::Unsupported);
    }

    Result<void> Spend(uint64 work) noexcept override {
        spent += work;
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    uint64 spent = 0;
    usize taken = 0;

private:
    Allocator& allocator_;

public:
    Array<StringView> recorded;
    Array<Name> tokens;
};

// Name 멤버 하나의 struct다(관찰 자리 방어 시험).
struct NameHolder {
    Name tag;
};

// String 값 하나(u64 길이 + byte)를 검사 없이 붙인다.
void AppendString(ArchiveWriter& out, StringView text) noexcept {
    if (!out.WriteU64(text.ByteCount()) ||
        !out.WriteRaw({reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()})) {
        platform::Fatal("test payload write failed");
    }
}

} // namespace

// 내용 규칙: 길이는 UE 5.8과 같다 — 번호 접미(SplitNameText)를 뗀 base가 1,023 UTF-16 단위
// 이하(보충 문자는 2단위)이고, 그 규칙의 최대 표시(3 byte 글자 1,023개 + `_2147483646` = 3,080 byte
// = MaxNameFieldBytes)는 받는다. 유효 UTF-8, NUL 없음, NFC(정규화하지 않는다 — 분해형은 거부). 빈
// 문자열(None)도 유효하다.
TEST(NameField_TextRulesFollowUeLengthAndRejectUtf8NulAndNonNfc) {
    SystemAllocator allocator;
    CHECK(TextError("", allocator) == ErrorCode::None);
    CHECK(TextError("Socket_7", allocator) == ErrorCode::None);
    CHECK(TextError("\xC3\xA9", allocator) == ErrorCode::None);

    // ASCII base: 1,023단위 허용·1,024 거부, 최대 번호 접미는 base에 세지 않는다. 번호가 아닌
    // 접미(선행 0·int32 최댓값)는 base에 들어간다.
    const Array<char> ascii = Repeat(allocator, "a", MaxNameBaseUtf16Units);
    const Array<char> asciiOver = Repeat(allocator, "a", MaxNameBaseUtf16Units + 1);
    CHECK(TextError(ViewOf(ascii), allocator) == ErrorCode::None);
    CHECK(TextError(ViewOf(asciiOver), allocator) == ErrorCode::InvalidArgument);
    CHECK(TextError(ViewOf(Concat(allocator, ascii, "_2147483646")), allocator) == ErrorCode::None);
    CHECK(TextError(ViewOf(Concat(allocator, asciiOver, "_1")), allocator) ==
          ErrorCode::InvalidArgument);
    CHECK(TextError(ViewOf(Concat(allocator, ascii, "_07")), allocator) ==
          ErrorCode::InvalidArgument);

    // 3 byte 글자 base(UE 이름 최대 3,069 byte): 최대 번호 접미까지 3,080 byte가 상한과 같다.
    const Array<char> wide = Repeat(allocator, "\xEA\xB0\x80", MaxNameBaseUtf16Units);
    const Array<char> widest = Concat(allocator, wide, "_2147483646");
    CHECK(widest.Size() == MaxNameFieldBytes && MaxNameFieldBytes == 3080);
    CHECK(TextError(ViewOf(widest), allocator) == ErrorCode::None);
    CHECK(TextError(ViewOf(Concat(allocator, wide, "_2147483647")), allocator) ==
          ErrorCode::InvalidArgument);
    const Array<char> wideOver = Repeat(allocator, "\xEA\xB0\x80", MaxNameBaseUtf16Units + 1);
    CHECK(TextError(ViewOf(wideOver), allocator) == ErrorCode::InvalidArgument);

    // 보충 문자(U+1F600)는 2단위다: 511개 + ASCII 하나 = 1,023단위 허용, 512개 = 1,024 거부.
    const Array<char> pairs = Repeat(allocator, "\xF0\x9F\x98\x80", 511);
    CHECK(TextError(ViewOf(Concat(allocator, pairs, "a")), allocator) == ErrorCode::None);
    CHECK(TextError(ViewOf(Repeat(allocator, "\xF0\x9F\x98\x80", 512)), allocator) ==
          ErrorCode::InvalidArgument);

    // byte 상한을 넘으면 base 규칙 전에 거부한다.
    CHECK(TextError(ViewOf(Repeat(allocator, "a", MaxNameFieldBytes + 1)), allocator) ==
          ErrorCode::InvalidArgument);
    CHECK(TextError(StringView("a\0b", 3), allocator) == ErrorCode::InvalidArgument);
    CHECK(TextError(StringView("\xFF", 1), allocator) == ErrorCode::InvalidArgument);
    // e + U+0301(결합 acute)는 NFD 형태다 — NFC는 U+00E9 한 글자다.
    CHECK(TextError("e\xCC\x81", allocator) == ErrorCode::InvalidArgument);
    CHECK(IsNoneNameText("") && IsNoneNameText("None") && IsNoneNameText("nONE") &&
          IsNoneNameText("_7"));
    CHECK(!IsNoneNameText("None_3") && !IsNoneNameText("Socket") && !IsNoneNameText("Nonex"));
}

// 검사 순서: byte 상한이 NFC 판정(할당)보다 먼저이고, NFC 판정의 할당 실패는 그 오류로 전파된다.
TEST(NameField_TextChecksLengthBeforeNfcAndPropagateAllocationFailure) {
    FailingAllocator failing;
    SystemAllocator allocator;
    const Array<char> longDecomposed = Repeat(allocator, "e\xCC\x81", 1027);
    REQUIRE(longDecomposed.Size() == MaxNameFieldBytes + 1);
    CHECK(TextError(ViewOf(longDecomposed), failing) == ErrorCode::InvalidArgument);
    CHECK(failing.attempts == 0);
    CHECK(TextError("Socket_7", failing) == ErrorCode::None);
    CHECK(failing.attempts == 0);
    CHECK(TextError("e\xCC\x81", failing) == ErrorCode::OutOfMemory);
    CHECK(failing.attempts != 0);
}

// 토큰 검사: 구조가 틀린 토큰은 pool 유무와 무관하게 InvalidArgument이고, 기본 None은 pool 없이 빈
// 표시 문자열이며, 그 밖은 pool 없음 Unsupported → 실행 중이 아닌 pool ShuttingDown → 다른 pool의
// domain WrongContext → 게시되지 않은 토큰 StaleHandle → 내용 규칙 InvalidArgument 순이다. 번호
// 없는 None 표시 변형은 그 pool의 토큰이어야 하고 빈 문자열로 저장한다. 번호 접미는 표시 문자열에
// 남는다.
TEST(NameField_TokenChecksPoolDomainAndDisplay) {
    SystemAllocator allocator;
    NamePool pool(allocator);
    NamePool other(allocator);
    REQUIRE(pool.Initialize());
    REQUIRE(other.Initialize());
    {
        CHECK(DisplayIs(nullptr, Name{}, "", allocator));
        CHECK(DisplayIs(&pool, Name{}, "", allocator));
        const Name socket = Intern(pool, "Socket_7");
        CHECK(TokenError(nullptr, socket, allocator) == ErrorCode::Unsupported);
        CHECK(TokenError(&other, socket, allocator) == ErrorCode::WrongContext);
        CHECK(DisplayIs(&pool, socket, "Socket_7", allocator));
        CHECK(socket.GetNumber() == 7);
        const Name upper = Intern(pool, "SOCKET_7");
        CHECK(upper == socket);
        CHECK(DisplayIs(&pool, upper, "SOCKET_7", allocator));
        const Name numberedNone = Intern(pool, "None_3");
        CHECK(DisplayIs(&pool, numberedNone, "None_3", allocator));

        // 번호 없는 None 표시 변형: 같은 pool이면 빈 문자열, pool이 없거나 다르면 기본 None과 달리
        // 거부한다.
        const Name lowerNone = Intern(pool, "none");
        REQUIRE(lowerNone.kind == NameKind::NoneDisplay && lowerNone.IsNone());
        CHECK(DisplayIs(&pool, lowerNone, "", allocator));
        CHECK(TokenError(nullptr, lowerNone, allocator) == ErrorCode::Unsupported);
        CHECK(TokenError(&other, lowerNone, allocator) == ErrorCode::WrongContext);

        // pool은 NUL·분해형·긴 원문도 intern하지만 필드 값으로는 거부한다.
        CHECK(TokenError(&pool, Intern(pool, StringView("a\0b", 3)), allocator) ==
              ErrorCode::InvalidArgument);
        CHECK(TokenError(&pool, Intern(pool, "e\xCC\x81"), allocator) ==
              ErrorCode::InvalidArgument);
        const Array<char> tooLong = Repeat(allocator, "b", MaxNameFieldBytes + 1);
        CHECK(TokenError(&pool, Intern(pool, ViewOf(tooLong)), allocator) ==
              ErrorCode::InvalidArgument);

        // 구조가 틀린 토큰과 같은 domain의 게시되지 않은 토큰이다.
        Name malformed = socket;
        malformed.id = 0;
        CHECK(TokenError(&pool, malformed, allocator) == ErrorCode::InvalidArgument);
        CHECK(TokenError(nullptr, malformed, allocator) == ErrorCode::InvalidArgument);

        // 실행 중이 아닌 pool(초기화 전·종료 뒤)은 domain 비교보다 먼저 ShuttingDown이다.
        NamePool idle(allocator);
        CHECK(TokenError(&idle, socket, allocator) == ErrorCode::ShuttingDown);
        NamePool closed(allocator);
        REQUIRE(closed.Initialize());
        const Name closedToken = Intern(closed, "Closed");
        REQUIRE(closed.Shutdown());
        CHECK(TokenError(&closed, closedToken, allocator) == ErrorCode::ShuttingDown);
        CHECK(TokenError(&closed, socket, allocator) == ErrorCode::ShuttingDown);
        Name unpublished = socket;
        unpublished.displayId = NumericLimits<uint64>::Max();
        CHECK(TokenError(&pool, unpublished, allocator) == ErrorCode::StaleHandle);
    }

    CHECK(pool.Shutdown());
    CHECK(other.Shutdown());
}

// FieldValue는 토큰을 그대로 싣고(할당 없음) 쓰기는 대입한다(pool 검사는 호출 경로 — 쓰기 thunk·
// 경로·FieldWriteOperation). 종류가 다르면 TypeMismatch다. wire는 단일 값 String, 원소 배열
// ValueArray다.
TEST(NameField_ValueConversionAndWire) {
    SystemAllocator allocator;
    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    {
        const Name token = Intern(pool, "Grip");
        FieldValue copied(allocator);
        REQUIRE(detail::CopyFieldValue(token, copied));
        CHECK(copied.kind == ValueKind::Name && copied.name == token &&
              copied.name.displayId == token.displayId);

        Name target{};
        REQUIRE(detail::WriteTargetValue<ValueKind::Name>(target, copied));
        CHECK(target == token);
        FieldValue wrong(allocator);
        wrong.kind = ValueKind::Utf8String;
        CHECK(!detail::WriteTargetValue<ValueKind::Name>(target, wrong));
        CHECK(target == token);

        auto single = detail::FieldWireType(ValueKind::Name, 0);
        CHECK(single && single.Value() == static_cast<uint32>(WireType::String));
        FieldSchema array;
        array.value = ValueType{ValueKind::Array, {}};
        array.element = ValueType{ValueKind::Name, {}};
        auto elements = detail::FieldWireType(array, 0);
        CHECK(elements && elements.Value() == static_cast<uint32>(WireType::ValueArray));
    }

    CHECK(pool.Shutdown());
}

// codec 단계: 1단계(기록하는 단계)는 표시 문자열을 검사·청구한 뒤 CollectName으로 방문 순서대로
// 기록하고 토큰은 기본 None이다. 2단계는 내용을 다시 보지 않고(청구·할당 없음 — 1단계 기록과 byte가
// 같음은 TakeName이 본다) 기록 순서의 토큰을 받는다. 다시 읽은 표시가 기록과 다르면
// InvalidArgument다. 단일 값과 Array<Name> 원소가 같은 순서 규칙이다.
TEST(NameField_DecodePhasesCollectThenTakeTokensInOrder) {
    SystemAllocator allocator;
    FailingAllocator failing;
    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    {
        FieldSchema single;
        single.id = Guid{0x4e46, 1};
        single.name = Span<const char>("label", 5);
        single.value = ValueType{ValueKind::Name, {}};
        ArchiveWriter payload(allocator);
        AppendString(payload, "Grip_2");
        const FieldView view{single.id, static_cast<uint32>(WireType::String), payload.View()};

        NameDecodeContext context(allocator, allocator);
        FieldValue value(allocator);
        {
            FieldScope scope(context, single, Guid{0x4e46, 9});
            REQUIRE(detail::ParseFieldValue(view, context, nullptr, &value));
        }

        REQUIRE(context.recorded.Size() == 1);
        CHECK(context.recorded[0] == StringView("Grip_2"));
        CHECK(value.kind == ValueKind::Name && value.name.kind == NameKind::None);
        CHECK(context.spent == 6);

        const Name grip = Intern(pool, "Grip_2");
        REQUIRE(context.tokens.TryPushBack(grip));
        context.EnterApplyPhase();
        {
            FieldScope scope(context, single, Guid{0x4e46, 9});
            REQUIRE(detail::ParseFieldValue(view, context, nullptr, &value));
        }

        CHECK(value.name == grip && value.name.displayId == grip.displayId);
        CHECK(context.spent == 6);
        // 기록이 다 소비된 뒤(또는 다른 표시)는 계약 위반이다.
        {
            FieldScope scope(context, single, Guid{0x4e46, 9});
            CHECK(!detail::ParseFieldValue(view, context, nullptr, &value));
        }

        // 2단계는 분해형 같은 내용도 다시 보지 않는다(NFC 판정 할당이 없다 — 1단계가 판정했다).
        NameDecodeContext unchecked(failing, allocator);
        unchecked.EnterApplyPhase();
        REQUIRE(unchecked.recorded.TryPushBack(StringView("e\xCC\x81")));
        REQUIRE(unchecked.tokens.TryPushBack(Name{}));
        ArchiveWriter decomposed(allocator);
        AppendString(decomposed, "e\xCC\x81");
        const FieldView raw{single.id, static_cast<uint32>(WireType::String), decomposed.View()};
        FieldValue applied(allocator);
        {
            FieldScope scope(unchecked, single, Guid{0x4e46, 9});
            CHECK(detail::ParseFieldValue(raw, unchecked, nullptr, &applied));
        }

        CHECK(failing.attempts == 0 && unchecked.spent == 0);

        // Array<Name>: 원소마다 같은 순서로 기록하고 받는다.
        FieldSchema array;
        array.id = Guid{0x4e46, 2};
        array.name = Span<const char>("names", 5);
        array.value = ValueType{ValueKind::Array, {}};
        array.element = ValueType{ValueKind::Name, {}};
        array.maxElements = DefaultMaxElements;
        ArchiveWriter elements(allocator);
        auto writer =
            ValueArrayWriter::Begin(elements, WireType::String, 2, NumericLimits<usize>::Max());
        REQUIRE(writer);
        AppendString(elements, "Hand");
        AppendString(elements, "Grip_2");
        REQUIRE(writer.Value().End());
        const FieldView arrayView{array.id, static_cast<uint32>(WireType::ValueArray),
                                  elements.View()};
        NameDecodeContext arrays(allocator, allocator);
        {
            FieldScope scope(arrays, array, Guid{0x4e46, 9});
            REQUIRE(detail::DecodeValueArray<Name>(nullptr, arrayView, arrays));
        }

        REQUIRE(arrays.recorded.Size() == 2);
        CHECK(arrays.recorded[0] == StringView("Hand") &&
              arrays.recorded[1] == StringView("Grip_2"));
        const Name hand = Intern(pool, "Hand");
        REQUIRE(arrays.tokens.TryPushBack(hand) && arrays.tokens.TryPushBack(grip));
        arrays.EnterApplyPhase();
        Array<Name> target(allocator);
        {
            FieldScope scope(arrays, array, Guid{0x4e46, 9});
            REQUIRE(detail::DecodeValueArray(&target, arrayView, arrays));
        }

        REQUIRE(target.Size() == 2);
        CHECK(target[0] == hand && target[1] == grip);
    }

    CHECK(pool.Shutdown());
}

// codec의 관찰 자리 방어: 1단계(기록하는 단계)에 값을 채우는 자리(관찰 struct 임시 값의 Name 멤버·
// Array<Name> 원소)는 토큰이 없으므로 InvalidArgument다(직렬화기는 Name을 담는 struct를 관찰
// 대상에서 먼저 거부한다).
TEST(NameField_CodecRejectsNameInObservationPositions) {
    SystemAllocator allocator;
    FieldSchema member;
    member.id = Guid{0x4e46, 3};
    member.name = Span<const char>("tag", 3);
    member.value = ValueType{ValueKind::Name, {}};
    ArchiveWriter payload(allocator);
    AppendString(payload, "Grip");
    const FieldView view{member.id, static_cast<uint32>(WireType::String), payload.View()};
    NameDecodeContext context(allocator, allocator);
    NameHolder holder;
    {
        FieldScope scope(context, member, Guid{0x4e46, 8});
        const RuntimeStructFieldOps ops{};
        auto decoded = detail::DecodeStructMember<NameHolder, &NameHolder::tag>(
            ops, nullptr, ObjectHandle{}, &holder, view, context);
        CHECK(!decoded && decoded.Error() == ErrorCode::InvalidArgument);
    }

    FieldSchema array;
    array.id = Guid{0x4e46, 4};
    array.name = Span<const char>("names", 5);
    array.value = ValueType{ValueKind::Array, {}};
    array.element = ValueType{ValueKind::Name, {}};
    array.maxElements = DefaultMaxElements;
    ArchiveWriter elements(allocator);
    auto writer =
        ValueArrayWriter::Begin(elements, WireType::String, 1, NumericLimits<usize>::Max());
    REQUIRE(writer);
    AppendString(elements, "Grip");
    REQUIRE(writer.Value().End());
    const FieldView arrayView{array.id, static_cast<uint32>(WireType::ValueArray), elements.View()};
    Array<Name> observed(allocator);
    {
        FieldScope scope(context, array, Guid{0x4e46, 8});
        auto decoded = detail::DecodeValueArray(&observed, arrayView, context);
        CHECK(!decoded && decoded.Error() == ErrorCode::InvalidArgument);
    }

    CHECK(context.recorded.IsEmpty() && observed.IsEmpty());
}
