#pragma once

#include "CoreMinimal.h"

// 교환 묶음 JSON writer다(UnrealExchange.ko.md 3.2절, 11.4절). UE 기본 JSON writer를 쓰지 않는 이유는
// 숫자 서식(%g 6자리)과 비유한값 처리(nan·inf가 그대로 나가 JSON이 깨짐) 때문이다.
//
// - 출력은 UTF-8, BOM 없음이다. 문자열은 RFC 8259대로 따옴표·역슬래시·제어 문자(U+0000–U+001F)만
//   escape하고 나머지는 UTF-8 그대로 쓴다. 짝 없는 UTF-16 surrogate는 U+FFFD로 바꾼다.
// - float는 %.9g, double은 %.17g(유효숫자 9·17자리, 다시 읽으면 같은 비트)다. 로캘과 무관하다
//   (std::to_chars). 비유한값은 문자열 "nan"·"inf"·"-inf"다.
// - Pretty이면 들여쓰기 두 칸·줄바꿈 LF이고 Finish가 끝에 LF를 하나 붙인다. 빈 객체·배열은 {}·[]다.
// - 키 순서는 호출 순서 그대로다(정렬은 호출자 책임 — 3.2절 표 순서).
//
// 사용 예:
//   FRocksJsonWriter Writer;
//   Writer.BeginObject();
//   Writer.Key(TEXT("formatVersion"));
//   Writer.Int(1);
//   Writer.EndObject();
//   TArray<uint8> Bytes = Writer.Finish();
//
// 잘못된 호출 순서(키 없이 객체에 값, 짝 안 맞는 End 등)는 check 실패다(프로그래밍 오류).
class ROCKSEXPORT_API FRocksJsonWriter
{
public:
	enum class EStyle : uint8
	{
		Pretty,
		Compact,
	};

	explicit FRocksJsonWriter(EStyle InStyle = EStyle::Pretty);

	void BeginObject();
	void EndObject();
	void BeginArray();
	void EndArray();

	// 객체 안에서 다음 값의 키를 쓴다.
	void Key(FStringView Name);

	void String(FStringView Value);
	void Bool(bool bValue);
	void Null();
	void Int(int64 Value);
	void UInt(uint64 Value);
	void Float(float Value);
	void Double(double Value);

	// 값 하나(최상위)를 다 썼는가다.
	bool IsComplete() const;

	// 완성된 bytes를 꺼낸다. 전제: IsComplete. 이 뒤 writer는 비어 있는 새 상태다.
	TArray<uint8> Finish();

	// 11.4절 숫자 토큰이다(유한값은 숫자, 비유한값은 따옴표 붙은 문자열). 시험·정렬 키용.
	static FString FormatFloat(float Value);
	static FString FormatDouble(double Value);

	// UTF-16 문자열을 UTF-8로 덧붙인다(짝 없는 surrogate는 U+FFFD).
	static void AppendUtf8(TArray<uint8>& Out, FStringView Text);
	static TArray<uint8> ToUtf8(FStringView Text);

	// 두 문자열의 UTF-8 byte 순 비교다(음수·0·양수). 3.2절 맵 정렬과 10.4절 6번 목록 정렬 기준이다.
	static int32 CompareUtf8(FStringView Left, FStringView Right);

private:
	struct FFrame
	{
		bool bObject = false;
		int32 Count = 0;
	};

	void BeforeValue();
	void NewLine(int32 Depth);
	void AppendAscii(const ANSICHAR* Text, int32 Length);
	void AppendQuoted(FStringView Text);

	TArray<uint8> Out;
	TArray<FFrame> Stack;
	EStyle Style;
	bool bKeyPending = false;
	bool bRootWritten = false;
};
