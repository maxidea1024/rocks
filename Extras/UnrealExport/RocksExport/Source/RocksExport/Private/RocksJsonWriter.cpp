#include "RocksJsonWriter.h"

#include <charconv>

namespace RocksJsonWriterPrivate
{
	static_assert(sizeof(TCHAR) == 2, "RocksExport assumes UTF-16 TCHAR.");

	// 11.4절 유효숫자 자리 수다(float·double 왕복 보장 최소 자리).
	static constexpr int32 FloatSignificantDigits = 9;
	static constexpr int32 DoubleSignificantDigits = 17;

	// to_chars 출력 버퍼 크기다(%.17g 최장 "-1.2345678901234567e-308" 24자에 여유).
	static constexpr int32 NumberBufferChars = 64;

	static constexpr uint32 ReplacementCharacter = 0xfffdu;
	static constexpr uint32 HighSurrogateFirst = 0xd800u;
	static constexpr uint32 LowSurrogateFirst = 0xdc00u;
	static constexpr uint32 SurrogateLast = 0xdfffu;
	static constexpr uint32 SupplementaryFirst = 0x10000u;
	static constexpr uint32 FirstNonControl = 0x20u;

	static constexpr int32 IndentSpaces = 2;

	// Text[Index]부터 code point 하나를 읽고 Index를 넘긴다. 짝 없는 surrogate는 U+FFFD다.
	static uint32 NextCodePoint(FStringView Text, int32& Index)
	{
		const uint32 First = uint32(Text[Index++]);
		if (First < HighSurrogateFirst || First > SurrogateLast)
		{
			return First;
		}

		if (First < LowSurrogateFirst && Index < Text.Len())
		{
			const uint32 Second = uint32(Text[Index]);
			if (Second >= LowSurrogateFirst && Second <= SurrogateLast)
			{
				++Index;
				return SupplementaryFirst + ((First - HighSurrogateFirst) << 10) + (Second - LowSurrogateFirst);
			}
		}

		return ReplacementCharacter;
	}

	static void AppendCodePoint(TArray<uint8>& Out, uint32 CodePoint)
	{
		if (CodePoint < 0x80u)
		{
			Out.Add(uint8(CodePoint));
		}
		else if (CodePoint < 0x800u)
		{
			Out.Add(uint8(0xc0u | (CodePoint >> 6)));
			Out.Add(uint8(0x80u | (CodePoint & 0x3fu)));
		}
		else if (CodePoint < SupplementaryFirst)
		{
			Out.Add(uint8(0xe0u | (CodePoint >> 12)));
			Out.Add(uint8(0x80u | ((CodePoint >> 6) & 0x3fu)));
			Out.Add(uint8(0x80u | (CodePoint & 0x3fu)));
		}
		else
		{
			Out.Add(uint8(0xf0u | (CodePoint >> 18)));
			Out.Add(uint8(0x80u | ((CodePoint >> 12) & 0x3fu)));
			Out.Add(uint8(0x80u | ((CodePoint >> 6) & 0x3fu)));
			Out.Add(uint8(0x80u | (CodePoint & 0x3fu)));
		}
	}

	// 유한값의 %.Ng 표기다(로캘 무관).
	template <typename T>
	static FString FormatFinite(T Value, int32 Digits)
	{
		char Buffer[NumberBufferChars];
		const std::to_chars_result Result = std::to_chars(Buffer, Buffer + NumberBufferChars, Value, std::chars_format::general, Digits);
		check(Result.ec == std::errc());
		return FString::ConstructFromPtrSize(Buffer, int32(Result.ptr - Buffer));
	}

	template <typename T>
	static FString FormatNumber(T Value, int32 Digits)
	{
		if (FMath::IsNaN(Value))
		{
			return TEXT("\"nan\"");
		}

		if (!FMath::IsFinite(Value))
		{
			return Value > 0 ? TEXT("\"inf\"") : TEXT("\"-inf\"");
		}

		return FormatFinite(Value, Digits);
	}

	template <typename T>
	static void AppendInteger(TArray<uint8>& Out, T Value)
	{
		char Buffer[NumberBufferChars];
		const std::to_chars_result Result = std::to_chars(Buffer, Buffer + NumberBufferChars, Value);
		check(Result.ec == std::errc());
		Out.Append(reinterpret_cast<const uint8*>(Buffer), int32(Result.ptr - Buffer));
	}
}

FRocksJsonWriter::FRocksJsonWriter(EStyle InStyle, int32 InBaseDepth)
	: Style(InStyle)
	, BaseDepth(InBaseDepth)
{
}

void FRocksJsonWriter::AppendAscii(const ANSICHAR* Text, int32 Length)
{
	Out.Append(reinterpret_cast<const uint8*>(Text), Length);
}

void FRocksJsonWriter::NewLine(int32 Depth)
{
	if (Style != EStyle::Pretty)
	{
		return;
	}

	Out.Add('\n');
	for (int32 Index = 0; Index < (Depth + BaseDepth) * RocksJsonWriterPrivate::IndentSpaces; ++Index)
	{
		Out.Add(' ');
	}
}

void FRocksJsonWriter::BeforeValue()
{
	if (Stack.IsEmpty())
	{
		checkf(!bRootWritten, TEXT("JSON writer: second root value"));
		bRootWritten = true;
		return;
	}

	FFrame& Frame = Stack.Last();
	if (Frame.bObject)
	{
		checkf(bKeyPending, TEXT("JSON writer: object value without key"));
		bKeyPending = false;
		return;
	}

	if (Frame.Count > 0)
	{
		Out.Add(',');
	}

	++Frame.Count;
	NewLine(Stack.Num());
}

void FRocksJsonWriter::Key(FStringView Name)
{
	checkf(!Stack.IsEmpty() && Stack.Last().bObject && !bKeyPending, TEXT("JSON writer: key outside object"));
	FFrame& Frame = Stack.Last();
	if (Frame.Count > 0)
	{
		Out.Add(',');
	}

	++Frame.Count;
	NewLine(Stack.Num());
	AppendQuoted(Name);
	Out.Add(':');
	if (Style == EStyle::Pretty)
	{
		Out.Add(' ');
	}

	bKeyPending = true;
}

void FRocksJsonWriter::BeginObject()
{
	BeforeValue();
	Out.Add('{');
	Stack.Add(FFrame{true, 0});
}

void FRocksJsonWriter::EndObject()
{
	checkf(!Stack.IsEmpty() && Stack.Last().bObject && !bKeyPending, TEXT("JSON writer: unbalanced object"));
	const FFrame Frame = Stack.Pop(EAllowShrinking::No);
	if (Frame.Count > 0)
	{
		NewLine(Stack.Num());
	}

	Out.Add('}');
}

void FRocksJsonWriter::BeginArray()
{
	BeforeValue();
	Out.Add('[');
	Stack.Add(FFrame{false, 0});
}

void FRocksJsonWriter::EndArray()
{
	checkf(!Stack.IsEmpty() && !Stack.Last().bObject, TEXT("JSON writer: unbalanced array"));
	const FFrame Frame = Stack.Pop(EAllowShrinking::No);
	if (Frame.Count > 0)
	{
		NewLine(Stack.Num());
	}

	Out.Add(']');
}

void FRocksJsonWriter::AppendQuoted(FStringView Text)
{
	using namespace RocksJsonWriterPrivate;

	static constexpr ANSICHAR HexDigits[] = "0123456789abcdef";
	Out.Add('"');
	for (int32 Index = 0; Index < Text.Len();)
	{
		const uint32 CodePoint = NextCodePoint(Text, Index);
		switch (CodePoint)
		{
		case '"':
			AppendAscii("\\\"", 2);
			break;
		case '\\':
			AppendAscii("\\\\", 2);
			break;
		case '\b':
			AppendAscii("\\b", 2);
			break;
		case '\f':
			AppendAscii("\\f", 2);
			break;
		case '\n':
			AppendAscii("\\n", 2);
			break;
		case '\r':
			AppendAscii("\\r", 2);
			break;
		case '\t':
			AppendAscii("\\t", 2);
			break;
		default:
			if (CodePoint < FirstNonControl)
			{
				const ANSICHAR Escape[] = {'\\', 'u', '0', '0', HexDigits[CodePoint >> 4], HexDigits[CodePoint & 0x0fu]};
				AppendAscii(Escape, UE_ARRAY_COUNT(Escape));
			}
			else
			{
				AppendCodePoint(Out, CodePoint);
			}

			break;
		}
	}

	Out.Add('"');
}

void FRocksJsonWriter::String(FStringView Value)
{
	BeforeValue();
	AppendQuoted(Value);
}

void FRocksJsonWriter::Bool(bool bValue)
{
	BeforeValue();
	if (bValue)
	{
		AppendAscii("true", 4);
	}
	else
	{
		AppendAscii("false", 5);
	}
}

void FRocksJsonWriter::Null()
{
	BeforeValue();
	AppendAscii("null", 4);
}

void FRocksJsonWriter::Int(int64 Value)
{
	BeforeValue();
	RocksJsonWriterPrivate::AppendInteger(Out, Value);
}

void FRocksJsonWriter::UInt(uint64 Value)
{
	BeforeValue();
	RocksJsonWriterPrivate::AppendInteger(Out, Value);
}

void FRocksJsonWriter::RawValue(TConstArrayView<uint8> Fragment)
{
	check(!Fragment.IsEmpty());
	BeforeValue();
	Out.Append(Fragment.GetData(), Fragment.Num());
}

void FRocksJsonWriter::Float(float Value)
{
	BeforeValue();
	AppendUtf8(Out, FormatFloat(Value));
}

void FRocksJsonWriter::Double(double Value)
{
	BeforeValue();
	AppendUtf8(Out, FormatDouble(Value));
}

bool FRocksJsonWriter::IsComplete() const
{
	return bRootWritten && Stack.IsEmpty() && !bKeyPending;
}

TArray<uint8> FRocksJsonWriter::Finish()
{
	checkf(IsComplete(), TEXT("JSON writer: incomplete document"));
	if (Style == EStyle::Pretty)
	{
		Out.Add('\n');
	}

	return FinishFragment();
}

TArray<uint8> FRocksJsonWriter::FinishFragment()
{
	checkf(IsComplete(), TEXT("JSON writer: incomplete document"));
	TArray<uint8> Result = MoveTemp(Out);
	Out.Reset();
	bRootWritten = false;
	return Result;
}

FString FRocksJsonWriter::FormatFloat(float Value)
{
	return RocksJsonWriterPrivate::FormatNumber(Value, RocksJsonWriterPrivate::FloatSignificantDigits);
}

FString FRocksJsonWriter::FormatDouble(double Value)
{
	return RocksJsonWriterPrivate::FormatNumber(Value, RocksJsonWriterPrivate::DoubleSignificantDigits);
}

void FRocksJsonWriter::AppendUtf8(TArray<uint8>& Target, FStringView Text)
{
	for (int32 Index = 0; Index < Text.Len();)
	{
		RocksJsonWriterPrivate::AppendCodePoint(Target, RocksJsonWriterPrivate::NextCodePoint(Text, Index));
	}
}

TArray<uint8> FRocksJsonWriter::ToUtf8(FStringView Text)
{
	TArray<uint8> Result;
	AppendUtf8(Result, Text);
	return Result;
}

int32 FRocksJsonWriter::CompareUtf8(FStringView Left, FStringView Right)
{
	// UTF-8 byte 순은 code point 순과 같다. 그래서 변환 없이 code point를 차례로 비교한다.
	int32 LeftIndex = 0;
	int32 RightIndex = 0;
	while (LeftIndex < Left.Len() && RightIndex < Right.Len())
	{
		const uint32 LeftCode = RocksJsonWriterPrivate::NextCodePoint(Left, LeftIndex);
		const uint32 RightCode = RocksJsonWriterPrivate::NextCodePoint(Right, RightIndex);
		if (LeftCode != RightCode)
		{
			return LeftCode < RightCode ? -1 : 1;
		}
	}

	const bool bLeftDone = LeftIndex >= Left.Len();
	const bool bRightDone = RightIndex >= Right.Len();
	if (bLeftDone == bRightDone)
	{
		return 0;
	}

	return bLeftDone ? -1 : 1;
}
