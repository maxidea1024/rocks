#pragma once

#include "CoreMinimal.h"

// FIPS 180-4 SHA-256이다(UnrealExchange 11.3절). blob 이름(내용 주소)과 Rocks 리더의 검증이 이
// 값을 쓴다. UE Core에는 공개 SHA-256이 없어(GetSHA256Signature는 구현 없음) 플러그인이 직접 둔다.
//
// 사용 예:
//   FRocksSha256 Hash;
//   Hash.Update(Bytes.GetData(), Bytes.Num());
//   const FString Hex = Hash.FinalizeHex();     // 소문자 64자
//
// 한 객체는 Finalize 뒤 Reset 전까지 다시 쓰지 않는다. 스레드 하나가 쓴다.
class ROCKSEXPORT_API FRocksSha256
{
public:
	static constexpr int32 DigestBytes = 32;
	static constexpr int32 BlockBytes = 64;
	static constexpr int32 HexChars = DigestBytes * 2;

	FRocksSha256();

	// 처음 상태로 되돌린다.
	void Reset();

	// 바이트를 더한다. Size가 0이면 아무 일도 하지 않는다(Data는 nullptr이어도 된다).
	void Update(const void* Data, uint64 Size);

	// 끝을 채우고 digest를 꺼낸다. 이 뒤에는 Reset해야 다시 쓸 수 있다.
	void Finalize(uint8 (&OutDigest)[DigestBytes]);

	// Finalize한 digest의 소문자 hex 64자다.
	FString FinalizeHex();

	// 한 번에 해시한 소문자 hex 64자다.
	static FString HashHex(const void* Data, uint64 Size);

	// digest를 소문자 hex로 바꾼다.
	static FString ToHex(const uint8 (&Digest)[DigestBytes]);

private:
	void Transform(const uint8* Block);

	uint32 State[8];
	uint8 Buffer[BlockBytes];
	uint64 TotalBytes = 0;
	int32 BufferedBytes = 0;
	bool bFinalized = false;
};
