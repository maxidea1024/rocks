#include "RocksSha256.h"

namespace RocksSha256Private
{
	// FIPS 180-4 4.2.2절 상수(처음 64개 소수의 세제곱근 소수부 32비트)다.
	static constexpr uint32 RoundConstants[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};

	// FIPS 180-4 5.3.3절 초기 해시 값이다.
	static constexpr uint32 InitialState[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};

	// 메시지 끝 표지(비트 1 하나)와 길이 필드(64비트 big-endian 비트 수)의 byte 크기다.
	static constexpr uint8 PaddingMarker = 0x80;
	static constexpr int32 LengthFieldBytes = 8;

	static uint32 RotateRight(uint32 Value, uint32 Count)
	{
		return (Value >> Count) | (Value << (32u - Count));
	}

	static uint32 LoadBigEndian32(const uint8* Bytes)
	{
		return (uint32(Bytes[0]) << 24) | (uint32(Bytes[1]) << 16) | (uint32(Bytes[2]) << 8) | uint32(Bytes[3]);
	}
}

FRocksSha256::FRocksSha256()
{
	Reset();
}

void FRocksSha256::Reset()
{
	FMemory::Memcpy(State, RocksSha256Private::InitialState, sizeof(State));
	FMemory::Memzero(Buffer, sizeof(Buffer));
	TotalBytes = 0;
	BufferedBytes = 0;
	bFinalized = false;
}

void FRocksSha256::Transform(const uint8* Block)
{
	using namespace RocksSha256Private;

	uint32 Schedule[64];
	for (int32 Index = 0; Index < 16; ++Index)
	{
		Schedule[Index] = LoadBigEndian32(Block + Index * 4);
	}

	for (int32 Index = 16; Index < 64; ++Index)
	{
		const uint32 S0 = RotateRight(Schedule[Index - 15], 7) ^ RotateRight(Schedule[Index - 15], 18) ^ (Schedule[Index - 15] >> 3);
		const uint32 S1 = RotateRight(Schedule[Index - 2], 17) ^ RotateRight(Schedule[Index - 2], 19) ^ (Schedule[Index - 2] >> 10);
		Schedule[Index] = Schedule[Index - 16] + S0 + Schedule[Index - 7] + S1;
	}

	uint32 A = State[0];
	uint32 B = State[1];
	uint32 C = State[2];
	uint32 D = State[3];
	uint32 E = State[4];
	uint32 F = State[5];
	uint32 G = State[6];
	uint32 H = State[7];
	for (int32 Index = 0; Index < 64; ++Index)
	{
		const uint32 Sum1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const uint32 Choose = (E & F) ^ (~E & G);
		const uint32 Temp1 = H + Sum1 + Choose + RoundConstants[Index] + Schedule[Index];
		const uint32 Sum0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temp2 = Sum0 + Majority;
		H = G;
		G = F;
		F = E;
		E = D + Temp1;
		D = C;
		C = B;
		B = A;
		A = Temp1 + Temp2;
	}

	State[0] += A;
	State[1] += B;
	State[2] += C;
	State[3] += D;
	State[4] += E;
	State[5] += F;
	State[6] += G;
	State[7] += H;
}

void FRocksSha256::Update(const void* Data, uint64 Size)
{
	check(!bFinalized);
	const uint8* Bytes = static_cast<const uint8*>(Data);
	TotalBytes += Size;

	// 버퍼에 남은 조각을 먼저 채운다.
	if (BufferedBytes > 0)
	{
		const uint64 Take = FMath::Min<uint64>(Size, uint64(BlockBytes - BufferedBytes));
		FMemory::Memcpy(Buffer + BufferedBytes, Bytes, Take);
		BufferedBytes += int32(Take);
		Bytes += Take;
		Size -= Take;
		if (BufferedBytes < BlockBytes)
		{
			return;
		}

		Transform(Buffer);
		BufferedBytes = 0;
	}

	while (Size >= uint64(BlockBytes))
	{
		Transform(Bytes);
		Bytes += BlockBytes;
		Size -= BlockBytes;
	}

	if (Size > 0)
	{
		FMemory::Memcpy(Buffer, Bytes, Size);
		BufferedBytes = int32(Size);
	}
}

void FRocksSha256::Finalize(uint8 (&OutDigest)[DigestBytes])
{
	using namespace RocksSha256Private;

	check(!bFinalized);
	const uint64 BitCount = TotalBytes * 8u;

	// 표지 byte, 0 채움, 길이 필드(5.1.1절). 길이 필드가 들어갈 자리가 없으면 블록을 하나 더 쓴다.
	Buffer[BufferedBytes++] = PaddingMarker;
	if (BufferedBytes > BlockBytes - LengthFieldBytes)
	{
		FMemory::Memzero(Buffer + BufferedBytes, BlockBytes - BufferedBytes);
		Transform(Buffer);
		BufferedBytes = 0;
	}

	FMemory::Memzero(Buffer + BufferedBytes, BlockBytes - LengthFieldBytes - BufferedBytes);
	for (int32 Index = 0; Index < LengthFieldBytes; ++Index)
	{
		Buffer[BlockBytes - 1 - Index] = uint8(BitCount >> (8 * Index));
	}

	Transform(Buffer);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutDigest[Index * 4 + 0] = uint8(State[Index] >> 24);
		OutDigest[Index * 4 + 1] = uint8(State[Index] >> 16);
		OutDigest[Index * 4 + 2] = uint8(State[Index] >> 8);
		OutDigest[Index * 4 + 3] = uint8(State[Index]);
	}

	bFinalized = true;
}

FString FRocksSha256::FinalizeHex()
{
	uint8 Digest[DigestBytes];
	Finalize(Digest);
	return ToHex(Digest);
}

FString FRocksSha256::HashHex(const void* Data, uint64 Size)
{
	FRocksSha256 Hash;
	Hash.Update(Data, Size);
	return Hash.FinalizeHex();
}

FString FRocksSha256::ToHex(const uint8 (&Digest)[DigestBytes])
{
	static constexpr TCHAR Digits[] = TEXT("0123456789abcdef");
	FString Hex;
	Hex.Reserve(HexChars);
	for (const uint8 Byte : Digest)
	{
		Hex.AppendChar(Digits[Byte >> 4]);
		Hex.AppendChar(Digits[Byte & 0x0f]);
	}

	return Hex;
}
