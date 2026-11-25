## 4. UE 5.8 조사

### 4.1 `EPixelFormat` 전체

`PF.h:16–115`에 0–93의 값과 `PF_MAX = 94`가 있다. 정보 표 `GPixelFormats`는 `PF.cpp:31–150`이다. 아래 표의 "블록"은 BlockSizeX×BlockSizeY·BlockBytes, "성분"은 NumComponents, "Core 지원"은 `PF.cpp` 마지막 열 `Supported`다(backend가 시작 때 덮어쓴다, 4.3절). BlockSizeZ는 모든 행이 1(`PF_Unknown`·`PF_PLATFORM_HDR_*`는 0)이다. "UE backend 대응"은 형식 표에 실제 장치 형식이 있는 backend와 그 줄이다. "Rocks"는 5절의 처리다.

| 값 | UE | 의미 | 블록 | 성분 | Core 지원 | UE backend 대응 | Rocks |
|---|---|---|---|---|---|---|---|
| 0 | `PF_Unknown` | 없음 | 0×0·0 | 0 | 0 | — | `Unknown` |
| 1 | `PF_A32B32G32R32F` | RGBA float32 | 1×1·16 | 4 | 1 | D12S:861, VK:772, GL:1002, MTL:800 | `RGBA32Float` |
| 2 | `PF_B8G8R8A8` | BGRA unorm8(메모리 B가 첫 바이트) | 1×1·4 | 4 | 1 | D12S:862(typeless), VK:766, GL:1073(RGBA8 + R·B swizzle), MTL:801 | `BGRA8Unorm` |
| 3 | `PF_G8` | 회색 unorm8. R 형식에 R → RGB 복제(`PF.h:21` 주석) | 1×1·1 | 1 | 1 | D12S:863, VK:767, GL:1076, MTL:802 | `R8Unorm`(합침) |
| 4 | `PF_G16` | 회색 unorm16 | 1×1·2 | 1 | 1 | D12S:864, VK:876·880, GL:1041(Android는 R16F 대체)·1043, MTL:803 | `R16Unorm`(합침) |
| 5 | `PF_DXT1` | BC1 | 4×4·8 | 3 | 1 | D12S:865, VK:849, GL:1102, MTL:914(macOS) | `BC1Unorm` |
| 6 | `PF_DXT3` | BC2 | 4×4·16 | 4 | 1 | D12S:866, VK:850, GL:1103, MTL:915 | `BC2Unorm` |
| 7 | `PF_DXT5` | BC3 | 4×4·16 | 4 | 1 | D12S:867, VK:851, GL:1104, MTL:916 | `BC3Unorm` |
| 8 | `PF_UYVY` | 4:2:2 packed YUV | 2×1·4 | 4 | 0 | 없음(D12S:869 UNKNOWN, D11:102 "Not supported", GL:1003 빈 값, MTL:978 Invalid) | 제외 |
| 9 | `PF_FloatRGB` | `PF_FloatR11G11B10`과 같음(`PF.h:27`) | 1×1·4 | 3 | 1 | D12S:903, VK:768(대체 사슬), GL:1063, MTL:896·918 | `RG11B10Ufloat`(합침) |
| 10 | `PF_FloatRGBA` | RGBA float16 | 1×1·8 | 4 | 1 | D12S:905, VK:769, GL:1065, MTL:979 | `RGBA16Float` |
| 11 | `PF_DepthStencil` | 플랫폼이 고르는 깊이 + 스텐실 | 1×1·4 | 1 | 0 | D12S:872·882, VK:835·842, GL:1006·1012, MTL:812·820 | `D24UnormS8Uint`·`D32FloatS8Uint`(명시) |
| 12 | `PF_ShadowDepth` | 플랫폼이 고르는 그림자 깊이 | 1×1·4 | 1 | 0 | D12S:890(D16), VK:770(D16), GL:1017(D16), MTL:902·949·954 | `D16Unorm`(명시) |
| 13 | `PF_R32_FLOAT` | R float32. 깊이 타깃 플래그와 함께면 D32F | 1×1·4 | 1 | 1 | D12S:893, VK:803, GL:1045, MTL:982 | `R32Float`(색), `D32Float`(깊이) |
| 14 | `PF_G16R16` | RG unorm16 | 1×1·4 | 2 | 1 | D12S:894, VK:773, GL:1089(데스크톱만), MTL:989 | `RG16Unorm` |
| 15 | `PF_G16R16F` | RG float16 | 1×1·4 | 2 | 1 | D12S:895, VK:774, GL:1046, MTL:994 | `RG16Float` |
| 16 | `PF_G16R16F_FILTER` | 15와 같은 저장 형식 | 1×1·4 | 2 | 1 | D12S:896, VK:775, GL:1047, MTL:995 | `RG16Float`(합침) |
| 17 | `PF_G32R32F` | RG float32 | 1×1·8 | 2 | 1 | D12S:897, VK:771, GL:1048, MTL:996 | `RG32Float` |
| 18 | `PF_A2B10G10R10` | RGB10 A2 unorm(DXGI R10G10B10A2) | 1×1·4 | 4 | 1 | D12S:898, VK:785, GL:1049, MTL:997 | `RGB10A2Unorm` |
| 19 | `PF_A16B16G16R16` | RGBA unorm16 | 1×1·8 | 4 | 1 | D12S:899, VK:786, GL:1019(RGBA16F로 다름), MTL:998 | `RGBA16Unorm`(합침) |
| 20 | `PF_D24` | 24비트 깊이 | 1×1·4 | 1 | 1 | D12S:900, VK:781(대체 사슬), GL:1018, MTL:905·960·964 | `D24Unorm`(명시) |
| 21 | `PF_R16F` | R float16 | 1×1·2 | 1 | 1 | D12S:901, VK:782, GL:1050, MTL:999 | `R16Float` |
| 22 | `PF_R16F_FILTER` | 21과 같은 저장 형식 | 1×1·2 | 1 | 1 | D12S:902, VK:783, GL:1051, MTL:1000 | `R16Float`(합침) |
| 23 | `PF_BC5` | BC5 | 4×4·16 | 2 | 1 | D12S:911, VK:853, GL:1083(데스크톱만), MTL:970 | `BC5Unorm` |
| 24 | `PF_V8U8` | RG snorm8 | 1×1·2 | 2 | 1 | D12S:910(SNORM), VK:802(UNORM으로 다름), GL:1082(데스크톱만), MTL:1001 | `RG8Snorm` |
| 25 | `PF_A1` | 1비트 알파 | 1×1·1 | 1 | 0 | D12S:912(R1_UNORM, Core 지원 0 유지), 그 밖 없음 | 제외 |
| 26 | `PF_FloatR11G11B10` | R11 G11 B10 unsigned float | 1×1·4 | 3 | 0(D12S:908 등이 1로 바꿈) | D12S:907, VK:784, GL:1064, MTL:898·922 | `RG11B10Ufloat` |
| 27 | `PF_A8` | 알파 unorm8 | 1×1·1 | 1 | 1 | D12S:913(A8_UNORM), VK:787(R8 + 000R), GL:1052(R8), MTL:1006(R8) | `A8Unorm` |
| 28 | `PF_R32_UINT` | R uint32 | 1×1·4 | 1 | 1 | D12S:914, VK:778, GL:1053, MTL:1007 | `R32Uint` |
| 29 | `PF_R32_SINT` | R sint32 | 1×1·4 | 1 | 1 | D12S:915, VK:779, GL:1054, MTL:1008 | `R32Sint` |
| 30 | `PF_PVRTC2` | PVRTC 2bpp | 8×4·8 | 4 | 0 | MTL:845(iOS)만 | 제외 |
| 31 | `PF_PVRTC4` | PVRTC 4bpp | 4×4·8 | 4 | 0 | MTL:847·849(iOS)만 | 제외 |
| 32 | `PF_R16_UINT` | R uint16 | 1×1·2 | 1 | 1 | D12S:916, VK:776, GL:1055, MTL:1017 | `R16Uint` |
| 33 | `PF_R16_SINT` | R sint16 | 1×1·2 | 1 | 1 | D12S:917, VK:777, GL:1056, MTL:1016 | `R16Sint` |
| 34 | `PF_R16G16B16A16_UINT` | RGBA uint16 | 1×1·8 | 4 | 1 | D12S:918, VK:795, GL:1021, MTL:1009 | `RGBA16Uint` |
| 35 | `PF_R16G16B16A16_SINT` | RGBA sint16 | 1×1·8 | 4 | 1 | D12S:919, VK:796, GL:1022, MTL:1010 | `RGBA16Sint` |
| 36 | `PF_R5G6B5_UNORM` | 16비트 묶음 R5 G6 B5 | 1×1·2 | 3 | 0(D12S:921 등이 1로) | D12S:920(B5G6R5), VK:788, GL:1031, MTL:909(iOS) | `R5G6B5Unorm` |
| 37 | `PF_R8G8B8A8` | RGBA unorm8 | 1×1·4 | 4 | 1 | D12S:924(typeless), VK:790, GL:1074, MTL:1011 | `RGBA8Unorm` |
| 38 | `PF_A8R8G8B8` | "legacy loading only, do NOT use"(`PF.h:56`) | 1×1·4 | 4 | 1 | MTL:1012만 | 제외 |
| 39 | `PF_BC4` | BC4 | 4×4·8 | 1 | 1 | D12S:868, VK:852, GL:1084(데스크톱만), MTL:968 | `BC4Unorm` |
| 40 | `PF_R8G8` | RG unorm8 | 1×1·2 | 2 | 1 | D12S:927, VK:801, GL:1058, MTL:1015 | `RG8Unorm` |
| 41 | `PF_ATC_RGB` | ATITC. "Unsupported Format"(`PF.h:59`) | 4×4·8 | 3 | 0 | 없음 | 제외 |
| 42 | `PF_ATC_RGBA_E` | 〃(`PF.h:60`) | 4×4·16 | 4 | 0 | 없음 | 제외 |
| 43 | `PF_ATC_RGBA_I` | 〃(`PF.h:61`) | 4×4·16 | 4 | 0 | 없음 | 제외 |
| 44 | `PF_X24_G8` | 깊이·스텐실 버퍼의 스텐실 읽기 SRV 전용. "Don't use for creating textures"(`PF.h:62`) | 1×1·1 | 1 | 0 | D12S:876·886, VK:836·843, GL:1016, MTL:815·823 | view plane으로 옮김(10절) |
| 45 | `PF_ETC1` | ETC1. "Unsupported Format"(`PF.h:63`) | 4×4·8 | 3 | 0 | 없음 | 제외 |
| 46 | `PF_ETC2_RGB` | ETC2 RGB8 | 4×4·8 | 3 | 0 | VK:869, GL:1109(Android), MTL:878 | `ETC2RGB8Unorm` |
| 47 | `PF_ETC2_RGBA` | ETC2 RGBA8(EAC 알파) | 4×4·16 | 4 | 0 | VK:870, GL:1110, MTL:880 | `ETC2RGBA8Unorm` |
| 48 | `PF_R32G32B32A32_UINT` | RGBA uint32 | 1×1·16 | 4 | 1 | D12S:928, VK:798, GL:1023, MTL:804 | `RGBA32Uint` |
| 49 | `PF_R16G16_UINT` | RG uint16 | 1×1·4 | 4(오기, 4.3절) | 1 | D12S:929, VK:793, GL:1027, MTL:805 | `RG16Uint` |
| 50 | `PF_ASTC_4x4` | ASTC LDR 4×4 | 4×4·16 | 4 | 0 | VK:860, GL:1117, MTL:851 | `ASTC4x4Unorm` |
| 51 | `PF_ASTC_6x6` | ASTC LDR 6×6 | 6×6·16 | 4 | 0 | VK:861, GL:1118, MTL:853 | `ASTC6x6Unorm` |
| 52 | `PF_ASTC_8x8` | ASTC LDR 8×8 | 8×8·16 | 4 | 0 | VK:862, GL:1119, MTL:855 | `ASTC8x8Unorm` |
| 53 | `PF_ASTC_10x10` | ASTC LDR 10×10 | 10×10·16 | 4 | 0 | VK:863, GL:1120, MTL:857 | `ASTC10x10Unorm` |
| 54 | `PF_ASTC_12x12` | ASTC LDR 12×12 | 12×12·16 | 4 | 0 | VK:864, GL:1121, MTL:859 | `ASTC12x12Unorm` |
| 55 | `PF_BC6H` | BC6H unsigned float | 4×4·16 | 3 | 1 | D12S:932(UF16), VK:854, GL:1087(데스크톱만), MTL:972 | `BC6HUfloat` |
| 56 | `PF_BC7` | BC7 | 4×4·16 | 4 | 1 | D12S:933(typeless), VK:855, GL:1086(데스크톱만), MTL:974 | `BC7Unorm` |
| 57 | `PF_R8_UINT` | R uint8 | 1×1·1 | 1 | 1 | D12S:934, VK:780, GL:1057, MTL:1018 | `R8Uint` |
| 58 | `PF_L8` | 휘도 8비트 | 1×1·1 | 1 | 0 | 없음(MTL:767 버퍼 표만) | 제외 |
| 59 | `PF_XGXR8` | 법선용 X·G·X·R 배치 | 1×1·4 | 4 | 1 | 없음 | 제외 |
| 60 | `PF_R8G8B8A8_UINT` | RGBA uint8 | 1×1·4 | 4 | 1 | D12S:925, VK:791, GL:1075, MTL:1013 | `RGBA8Uint` |
| 61 | `PF_R8G8B8A8_SNORM` | RGBA snorm8 | 1×1·4 | 4 | 1 | D12S:926, VK:792, GL:1061(RGBA8 unorm으로 대체), MTL:1014 | `RGBA8Snorm` |
| 62 | `PF_R16G16B16A16_UNORM` | RGBA unorm16 | 1×1·8 | 4 | 1 | D12S:936, VK:800, GL:1024, MTL:1021 | `RGBA16Unorm` |
| 63 | `PF_R16G16B16A16_SNORM` | RGBA snorm16 | 1×1·8 | 4 | 1 | D12S:937, VK:799, GL:1025(RGBA16 + SHORT), MTL:1022 | `RGBA16Snorm` |
| 64 | `PF_PLATFORM_HDR_0` | 플랫폼 HDR 출력 형식 | 0×0·0 | 0 | 0 | MTL:893·933(BGR10_XR_sRGB)·943(RGBA16Float)만 | 제외 |
| 65 | `PF_PLATFORM_HDR_1` | "Reserved"(`PF.h:83`) | 0×0·0 | 0 | 0 | 없음 | 제외 |
| 66 | `PF_PLATFORM_HDR_2` | "Reserved"(`PF.h:84`) | 0×0·0 | 0 | 0 | 없음 | 제외 |
| 67 | `PF_NV12` | 4:2:0 8비트 2-plane(R8 휘도 + RG8 ¼ 색차, `PF.cpp:110–111`) | 1×1·1(의미 없음) | 1 | 0 | D12S:938, D11:177 | `NV12` |
| 68 | `PF_R32G32_UINT` | RG uint32 | 1×1·8 | 2 | 1 | D12S:931, VK:797, GL:1059, MTL:807 | `RG32Uint` |
| 69 | `PF_ETC2_R11_EAC` | EAC R11 | 4×4·8 | 1 | 0 | VK:871, GL:1111, MTL:882 | `EACR11Unorm` |
| 70 | `PF_ETC2_RG11_EAC` | EAC RG11 | 4×4·16 | 2 | 0 | VK:872, GL:1112, MTL:884 | `EACRG11Unorm` |
| 71 | `PF_R8` | R unorm8 | 1×1·1 | 1 | 1 | D12S:935, VK:804, GL:1029, MTL:1019 | `R8Unorm` |
| 72 | `PF_B5G5R5A1_UNORM` | 16비트 묶음 B5 G5 R5 A1(DXGI 배치) | 1×1·2 | 4 | 0(D12S:923 등이 1로) | D12S:922, VK:789(대체 사슬), GL:1037, MTL:911(iOS) | `B5G5R5A1Unorm` |
| 73 | `PF_ASTC_4x4_HDR` | ASTC HDR 4×4 | 4×4·16 | 4 | 0 | VK:885, GL:1125, MTL:865 | `ASTC4x4Hdr` |
| 74 | `PF_ASTC_6x6_HDR` | ASTC HDR 6×6 | 6×6·16 | 4 | 0 | VK:886, GL:1126, MTL:867 | `ASTC6x6Hdr` |
| 75 | `PF_ASTC_8x8_HDR` | ASTC HDR 8×8 | 8×8·16 | 4 | 0 | VK:887, GL:1127, MTL:869 | `ASTC8x8Hdr` |
| 76 | `PF_ASTC_10x10_HDR` | ASTC HDR 10×10 | 10×10·16 | 4 | 0 | VK:888, GL:1128, MTL:871 | `ASTC10x10Hdr` |
| 77 | `PF_ASTC_12x12_HDR` | ASTC HDR 12×12 | 12×12·16 | 4 | 0 | VK:889, GL:1129, MTL:873 | `ASTC12x12Hdr` |
| 78 | `PF_G16R16_SNORM` | RG snorm16 | 1×1·4 | 2 | 1 | D12S:940, VK:805, GL:1090(데스크톱만), MTL:1027 | `RG16Snorm` |
| 79 | `PF_R8G8_UINT` | RG uint8 | 1×1·2 | 2 | 1 | D12S:941, VK:806, GL:1067, MTL:1028 | `RG8Uint` |
| 80 | `PF_R32G32B32_UINT` | RGB uint32 | 1×1·12 | 3 | 1 | D12S:942, VK:807, GL:1068, MTL:1029(Invalid) | `RGB32Uint` |
| 81 | `PF_R32G32B32_SINT` | RGB sint32 | 1×1·12 | 3 | 1 | D12S:943, VK:808, GL:1069, MTL:1031(Invalid) | `RGB32Sint` |
| 82 | `PF_R32G32B32F` | RGB float32 | 1×1·12 | 3 | 1 | D12S:944, VK:809, GL:1070, MTL:1033(Invalid) | `RGB32Float` |
| 83 | `PF_R8_SINT` | R sint8 | 1×1·1 | 1 | 1 | D12S:945, VK:810, GL:1071, MTL:1035 | `R8Sint` |
| 84 | `PF_R64_UINT` | R uint64(64비트 image atomic) | 1×1·8 | 1 | 0 | VK:822(SM5, R32G32 대체), MTL:1036(Invalid) | `R64Uint` |
| 85 | `PF_R9G9B9EXP5` | RGB9 E5 공유 지수 | 1×1·4 | 4 | 0 | D12S:946만(VK·GL 없음, MTL:1038 Invalid) | `RGB9E5Ufloat` |
| 86 | `PF_P010` | 4:2:0 10비트 2-plane(R16 + RG16 ¼, 상위 10비트, `PF.cpp:137–138`) | 1×1·2(의미 없음) | 1 | 0 | D12S:947, D11:187 | `P010` |
| 87 | `PF_ASTC_4x4_NORM_RG` | ASTC를 LA 끝점으로 인코딩, 실행 때 RG로 swizzle(`PF.h:105`) | 4×4·16 | 2 | 0 | 없음 | ASTC + swizzle로 옮김 |
| 88 | `PF_ASTC_6x6_NORM_RG` | 〃 | 6×6·16 | 2 | 0 | 없음 | 〃 |
| 89 | `PF_ASTC_8x8_NORM_RG` | 〃 | 8×8·16 | 2 | 0 | 없음 | 〃 |
| 90 | `PF_ASTC_10x10_NORM_RG` | 〃 | 10×10·16 | 2 | 0 | 없음 | 〃 |
| 91 | `PF_ASTC_12x12_NORM_RG` | 〃 | 12×12·16 | 2 | 0 | 없음 | 〃 |
| 92 | `PF_R16G16_SINT` | RG sint16 | 1×1·4 | 4(오기, 4.3절) | 1 | D12S:930, VK:794, GL:1028, MTL:806 | `RG16Sint` |
| 93 | `PF_R8G8B8` | RGB unorm8(24비트) | 1×1·3 | 3 | 0 | VK:811(샘플·RT 필수), GL:1077 | `RGB8Unorm` |

UE 블록 압축 판정 함수는 셋이다. BC 계열 `IsBlockCompressedFormat`(`PF.h:322–338`), ASTC 계열(`PF.h:295–320`), ETC 계열(`PF.h:280–293`)이다. BC만 최상위 크기가 4의 배수여야 한다(`RequiresBlock4Alignment`, `PF.h:451–455`). 깊이·스텐실 판정은 `PF.h:403–430`이며 `PF_R32_FLOAT`도 깊이로 본다.

### 4.2 UE가 레거시·미지원·플랫폼 전용으로 다루는 것

| 형식 | UE 취급 | 근거 |
|---|---|---|
| `PF_A8R8G8B8` | 옛 데이터 읽기 전용, 쓰지 말 것 | `PF.h:56` 주석, `CoreUObject/Public/UObject/NoExportTypes.h:252`. Metal만 대응(MTL:1012). 엔진 코드 한 곳이 셰이더 출력 형식 이름으로 쓴다(`Engine/Private/Particles/ParticleGpuSimulation.cpp:1816`) |
| `PF_ATC_RGB`·`PF_ATC_RGBA_E`·`PF_ATC_RGBA_I` | 미지원 형식 | `PF.h:59–61` 주석, Core 지원 0(`PF.cpp:79–81`), 대응 backend 없음 |
| `PF_ETC1` | 미지원 형식 | `PF.h:63` 주석, Core 지원 0(`PF.cpp:83`), 대응 backend 없음 |
| `PF_PVRTC2`·`PF_PVRTC4` | iOS Metal 전용 | Core 지원 0(`PF.cpp:66–67`), iOS 분기에서만 지원(MTL:830–850). 압축 모듈이 없다(`Developer/`의 `TextureFormat*`는 ASTC·DXT·ETC2·IntelISPCTexComp·Oodle·Uncompressed뿐) |
| `PF_PLATFORM_HDR_0` | 플랫폼 HDR 출력용 | Metal이 HDR 출력 지원 여부로 켠다(MTL:887–894, 933–943). 블록 정보도 Core 표에 없음(`PF.cpp:106`) |
| `PF_PLATFORM_HDR_1`·`_2` | 예약 | `PF.h:83–84` 주석 |
| `PF_UYVY` | 어느 RHI도 지원하지 않음 | D3D11 "TODO: Not supported"(D11:102), D3D12 UNKNOWN(D12S:869), GL 빈 값(GL:1003), Metal Invalid(MTL:978) |
| `PF_A1` | 지원 안 함 | Core 지원 0(`PF.cpp:59`)이고 어느 backend도 1로 바꾸지 않는다. 크기 계산만 특례(`RHI.cpp:1780–1786`) |
| `PF_L8` | 지원 안 함 | Core 지원 0(`PF.cpp:99`), backend 대응 없음, 채널 플래그 None(`RU:334`) |
| `PF_XGXR8` | 장치 형식 없음 | backend 대응 없음. 쿠킹 형식 이름 `XGXR8`은 `PF_B8G8R8A8`로 출력된다(`Developer/TextureFormatUncompressed/Private/TextureFormatUncompressed.cpp:134–137`) |
| `PF_X24_G8` | 텍스처 생성 금지, 스텐실 읽기 view 전용 | `PF.h:62` 주석, view 생성이 plane을 스텐실로 변환한다(`RHI/Private/RHIResources.cpp:348–372`) |
| `PF_ASTC_*_NORM_RG` | RHI의 swizzle 지원이 필요 | `PF.h:105` 주석. 쿠킹 형식 `ASTC_NormalRG_Precise`가 6×6만 낸다(`Developer/TextureFormatASTC/Private/TextureFormatASTC.cpp:106, 284–288`). 공개 RHI 형식 표 어디에도 장치 형식이 없다(Metal 버퍼 표 MTL:790–794는 Invalid) |
| `PF_G16R16F_FILTER`·`PF_R16F_FILTER` | 필터 가능 표시용 별칭 | 모든 backend가 비필터 형식과 같은 장치 형식을 준다(D12S:896·902, VK:775·783, GL:1047·1051, MTL:995·1000) |
| `PF_FloatRGB` | `PF_FloatR11G11B10`과 같음 | `PF.h:27` 주석, `NoExportTypes.h:217` |

### 4.3 정보 표(`FPixelFormatInfo`)

필드(`PF.h:462–512`):
- 이름 `Name`, 형식 값 `UnrealFormat`
- `BlockSizeX`·`BlockSizeY`·`BlockSizeZ`·`BlockBytes`·`NumComponents`
- 장치 형식 `PlatformFormat`(RHI가 채움), 능력 `Capabilities`(RHI가 채움), 지원 여부 `Supported`(1비트), 24비트 unorm 깊이 여부 `bIs24BitUnormDepthStencil`

크기 계산은 블록 수 ceil(폭/블록 폭) × ceil(높이/블록 높이) × 블록 바이트이고(`PF.cpp:186–218`) 깊이 방향 블록은 없다(`PF.cpp:158` 주석). mip 크기는 축마다 max(1, n >> mip)이다(`PF.cpp:220–239`). 채널 플래그는 정보 표가 아니라 별도 표 `GetPixelFormatValidChannels`(`RU:272–371`)에 있다.

**UE 표의 오기.** Rocks 정보 표는 아래를 따르지 않는다.
- `PF_R16G16_UINT`·`PF_R16G16_SINT`의 성분 수가 4다(`PF.cpp:87, 148`). 채널 표는 RG다(`RU:325, 368`).
- `PF_R16G16B16A16_UNORM`·`_SNORM`의 이름 문자열이 `"R16G16B16A16_UINT"`·`"R16G16B16A16_SINT"`다(`PF.cpp:104–105`).
- 채널 표가 `PF_R32_SINT`를 RGBA로 기재한다(`RU:305`).
- Vulkan이 `PF_V8U8`(snorm)을 `R8G8_UNORM`에 대응한다(VK:802). D3D12는 `R8G8_SNORM`이다(D12S:910).
- OpenGL이 `PF_A16B16G16R16`을 `GL_RGBA16F`에 대응한다(GL:1019). 같은 의미의 `PF_R16G16B16A16_UNORM`은 `GL_RGBA16`이다(GL:1024).
- OpenGL이 `PF_R16G16B16A16_SNORM`을 `GL_RGBA16`(unorm) + `GL_SHORT`로 대응한다(GL:1025).

### 4.4 형식 능력(`EPixelFormatCapabilities`)과 판정

플래그(`PF.h:234–272`): `Texture1D`·`Texture2D`·`Texture3D`·`TextureCube`·`RenderTarget`·`DepthStencil`·`TextureMipmaps`·`TextureLoad`·`TextureSample`·`TextureGather`·`TextureAtomics`·`TextureBlendable`·`TextureStore`·`Buffer`·`VertexBuffer`·`IndexBuffer`·`BufferLoad`·`BufferStore`·`BufferAtomics`·`UAV`·`TypedUAVLoad`·`TypedUAVStore`·`TextureFilterable`·`LossyCompressible`. 조회는 `UE::PixelFormat::HasCapabilities`(`PF.h:531–537`)다. MSAA 표본 수는 이 플래그에 없다.

| backend | 판정 방식 | 위치 |
|---|---|---|
| D3D12 | 장치 생성 뒤 형식마다 `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`를 원 형식·SRV 형식·UAV 형식·DSV 형식으로 각각 질의하고 `D3D12_FORMAT_SUPPORT1/2` 비트를 옮긴다. `TextureFilterable`은 `SHADER_SAMPLE`에서 함께 켠다(별도 비트 없음) | D12D:192–270, 호출 `D3D12RHI/Private/D3D12Adapter.cpp:1562` |
| D3D11 | D3D12와 같은 구조로 `CheckFormatSupport`·`FormatSupport2`를 질의 | D11:457–524 |
| Vulkan | 형식마다 우선순위 목록의 VkFormat을 차례로 `vkGetPhysicalDeviceFormatProperties`로 질의하고 요구 능력을 만족하는 첫 형식을 고른다. `optimalTilingFeatures`·`linearTilingFeatures`·`bufferFeatures` 비트를 옮기고 mip은 `vkGetPhysicalDeviceImageFormatProperties`의 `maxMipLevels > 1`, gather는 `shaderImageGatherExtended`로 정한다. 지원 여부는 기본 요구(`TextureSample`) 또는 버퍼 능력이다 | VK:730–897(대응), 1080–1250(질의), 1222(기본 요구), 1239(지원 판정) |
| OpenGL | 질의하지 않는다. 내부 형식 enum별 정적 분류(Texture·Render·Filterable·Image·DepthStencil)를 능력으로 옮긴다 | GL:384–500(분류), 502–559(옮김) |
| Metal | 기본 능력(`RHIInitDefaultPixelFormatCapabilities`)을 준 뒤 GPU family별로 몇 형식의 능력을 수정한다 | MTL:1049–1062, 기본 능력 `RHI.cpp:1752–1773` |

기본 능력 규칙(`RHI.cpp:1752–1773`): 지원 형식 가운데 BC 계열은 텍스처·mip·load·sample·gather·filter만, 그 밖은 모든 텍스처·버퍼·UAV 플래그를 주고 깊이 형식이 아니면 `DepthStencil`을 제외한다.

### 4.5 backend 대응

**D3D12.**
- 형식 표는 `InitializeD3D12PixelFormats`(D12S:858–949)다. 표에 없는 형식은 `PlatformFormat = 0`(`DXGI_FORMAT_UNKNOWN`)이다. ETC2·EAC·ASTC·PVRTC·ATC·`PF_R8G8B8`·`PF_R64_UINT`·`PF_L8`·`PF_XGXR8`·`PF_A8R8G8B8`이 여기에 해당한다.
- sRGB 짝이 있는 형식은 typeless로 생성한다: `B8G8R8A8_TYPELESS`·`R8G8B8A8_TYPELESS`·`BC1/2/3/7_TYPELESS`(D12S:862, 865–867, 924, 933).
- 깊이 형식도 typeless다: `R16_TYPELESS`(D16), `R24G8_TYPELESS`(D24S8), `R32G8X24_TYPELESS`(D32FS8)(D12S:872–890, 900).
- view 형식 결정 함수:
  - `FindShaderResourceFormat(format, bSRGB)`: typeless → UNORM 또는 UNORM_SRGB, 깊이 typeless → 깊이 읽기 형식(`DXGI:86–120`)
  - `FindDepthStencilFormat`: typeless → DSV 형식(`DXGI:166–176`)
  - `FindUnorderedAccessFormat`: typeless → UNORM, D32FS8은 `R32_FLOAT_X8X24_TYPELESS`(`DXGI:154–163`)
  - `GetPlatformTextureResourceFormat`: 깊이 타깃 플래그면 `R32_FLOAT` → `R32_TYPELESS` 등으로 변환한다(`DXGI:122–151`). `PF_R32_FLOAT`가 D32F 깊이로 쓰이는 경로다
  - `GetPlaneSliceFromViewFormat`: 깊이·스텐실·NV12·P010의 plane 번호(`DXGI:217–256`)
- 텍스처 생성: 자원 형식(`D3D12RHI/Private/D3D12Texture.cpp:230, 599`), DSV·RTV 형식(`:604–612`), SRV(`:1169`, `D3D12RHI/Private/D3D12SRV.cpp:170–180`), UAV(`D3D12RHI/Private/D3D12UAV.cpp:155–160`).
- 깊이 기본은 32비트 float + 스텐실이다(`r.D3D12.Depth24Bit` 기본 0, `D3D12RHI/Private/D3D12RHI.cpp:39–44`. 도움말 문자열은 1을 "(default)"로 적어 값과 다르다).

**D3D11.** D3D12와 같은 DXGI 유틸리티를 쓴다(`Windows/D3D11RHI/Private/D3D11RHI.cpp`, `D3D11SRV.cpp`). 형식 표(D11:92–188)의 차이는 다음과 같다.
- `PF_R9G9B9EXP5`를 대응하지 않는다(D3D12는 D12S:946).
- NV12·P010 텍스처에 SRV를 만들지 않는다(`Windows/D3D11RHI/Private/D3D11Texture.cpp:325–331`).
- 깊이 기본값은 D3D12와 같다(`r.D3D11.Depth24Bit` 기본 0, D11:29–34).

**Vulkan.**
- 대응은 `MapFormatSupport(UE 형식, {우선순위 VkFormat…}, component mapping, 요구 능력, 블록 바이트)`(VK:765–890, 정의 1226–1250)다. 첫 VkFormat이 요구를 못 채우면 다음 형식으로 대체하고 로그만 남긴다(VK:1211–1214). 대체 사슬이 있는 형식은 `PF_FloatRGB`·`PF_FloatR11G11B10`(→ R16G16B16 → RGBA16F, VK:768, 784), `PF_G16R16`·`PF_A16B16G16R16`·`PF_R16G16B16A16_UNORM/SNORM`(→ float16, VK:773, 786, 799–800), `PF_B5G5R5A1_UNORM`(→ BGRA8, VK:789), `PF_D24`(VK:781), 깊이·스텐실(VK:835–843), `PF_R64_UINT`(→ R32G32, VK:822)이다.
- component mapping으로 채널 의미를 구성한다. `PF_G8`은 R001(VK:767), `PF_A8`은 000R(VK:787), 스텐실 view는 RR01(VK:836)이다.
- 압축 계열은 플랫폼 hook으로 켠다. Windows 등 일반 플랫폼은 BC만(`VulkanRHI/Private/VulkanGenericPlatform.h:76–79`), Android는 ASTC·ETC2 항상, BC는 `r.Vulkan.SupportsBCTextureFormats`일 때(`VulkanRHI/Private/Android/VulkanAndroidPlatform.h:137–141`, `VulkanAndroidPlatform.cpp:71–72, 433–436`). ASTC HDR은 `VK_EXT_texture_compression_astc_hdr`일 때다(VK:883–890).
- 형식 질의는 4.4절 표와 같다.

**OpenGL(ES).**
- 형식 표는 `SetupTextureFormat(UE 형식, {internal, internal sRGB, format, type, 압축, BGRA})`(GL:992–1130)다. 구조체는 `OpenGLDrv/Private/OpenGLDrvPrivate.h:234–241`이고 `InternalFormat[0]`이 선형, `[1]`이 sRGB다.
- 플랫폼 분기가 많다.
  - 데스크톱(`PLATFORM_DESKTOP`)만: BC4·BC5·BC6H·BC7(RGTC·BPTC), `PF_V8U8`, `PF_G16R16`·`PF_G16R16_SNORM`(GL:1078–1091).
  - S3TC(BC1–3)는 `SupportsDXT`일 때다. 데스크톱 기본 클래스는 true(`OpenGLDrv/Private/OpenGLBase.h:95`), ES 클래스는 false(`OpenGLES.h:67`).
  - ETC2·EAC는 Android에서만 등록한다(GL:1106–1114, `OpenGLES.cpp:241`).
  - ASTC LDR은 `GL_KHR_texture_compression_astc_ldr`, ASTC HDR은 거기에 `_hdr`까지 있을 때다(`OpenGLDrv/Private/OpenGLDrv.cpp:294–296`). ASTC HDR도 LDR과 같은 internal format enum을 쓴다(GL:1125–1129).
  - Android는 `PF_G16`을 `GL_R16F`로 대체한다(GL:1039–1041).
  - ES에는 `GL_UNSIGNED_SHORT_1_5_5_5_REV`가 없어 `PF_B5G5R5A1_UNORM`을 `5_5_5_1` + R·B swizzle로 대응한다(GL:1033–1037, `OpenGL3.h:628`). 비트 배치가 실제로 일치하는지는 **미확인**이다(5_5_5_1은 알파가 최하위 비트다).
- Windows의 `GLES31_PC` 미리보기는 ES context가 아니라 데스크톱 GL 4.x context(`FWindowsOpenGL : FOpenGL4`, `OpenGLDrv/Private/Windows/WindowsOpenGL.h:19`)에서 위 데스크톱 분기를 탄다. Android는 `FAndroidOpenGL : FOpenGLES`(`OpenGLDrv/Private/Android/AndroidOpenGL.h:14`)다.
- sRGB는 생성 때 `InternalFormat[bSRGB]`로 정해진다(`OpenGLDrv/Private/OpenGLTexture.cpp:584–589`). SRV가 다른 형식을 요구하면 실패한다. "texture views are unsupported"(`OpenGLDrv/Private/OpenGLUAV.cpp:122–124`).
- 깊이 기본은 24비트다(`r.OpenGL.Depth24Bit` 기본 1, GL:72–76). `PF_D24`는 ES에서 `GL_DEPTH_COMPONENT24`(`OpenGLES.h:69`), 데스크톱 기본 클래스에서 `GL_DEPTH_COMPONENT16`(`OpenGLBase.h:117`)이다.

**OpenGL ES 3.2 사양 확인(Rocks 대응의 근거).**
- 색 형식(Table 8.10, `ES32` 162–163쪽):
  - color-renderable: R8, RG8, RGB8, RGB565, RGBA4, RGB5_A1, RGBA8, RGB10_A2, RGB10_A2UI, SRGB8_ALPHA8, R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F, R11F_G11F_B10F, 그리고 RGB 3성분이 아닌 정수 형식(R·RG·RGBA의 8I·8UI·16I·16UI·32I·32UI)이다. float 형식의 렌더 타깃이 ES 3.2 core다.
  - texture-filterable: unorm·snorm 8비트 형식 전부, RGB565, RGBA4, RGB5_A1, RGB10_A2, SRGB8, SRGB8_ALPHA8, R16F, RG16F, RGB16F, RGBA16F, R11F_G11F_B10F, RGB9_E5다. 32비트 float(R32F·RG32F·RGB32F·RGBA32F)는 filterable이 아니다.
  - 렌더 타깃이 아닌 형식: snorm 8비트, SRGB8, RGB16F, RGB32F, RGB9_E5, RGB 3성분 정수 형식(RGB8I…RGB32UI).
- 깊이·스텐실(Table 8.11, 164쪽): DEPTH_COMPONENT16·24·32F, DEPTH24_STENCIL8, DEPTH32F_STENCIL8, STENCIL_INDEX8.
- 압축(Table 8.17, 178–179쪽): ETC2·EAC 전 계열과 ASTC LDR 2D 블록 14종(4×4–12×12)과 각 sRGB가 core다. 3D 텍스처 열이 모두 비어 있어 ETC2·ASTC 3D 텍스처는 core가 아니다. cube map array는 PUNCHTHROUGH 두 형식을 뺀 전부가 가능하다.
- image load/store 형식(Table 8.26, 219–220쪽): RGBA32F, RGBA16F, R32F, RGBA32UI, RGBA16UI, RGBA8UI, R32UI, RGBA32I, RGBA16I, RGBA8I, R32I, RGBA8, RGBA8_SNORM.
- `DEPTH_STENCIL_TEXTURE_MODE`는 core 텍스처 파라미터다(Table 8.19, 189쪽). 사양 본문에 texture view가 없다(전문 검색 0건). 확장은 `GL_OES_texture_view`·`GL_EXT_texture_view`다(`gl2ext.h:726, 2204`).
- 확장 이름은 NDK 헤더로 확인하였다.
  - `GL_EXT_texture_compression_s3tc`(`gl2ext.h:2080`), `_s3tc_srgb`(`:2086`), `_rgtc`(`:2072`), `_bptc`(`:2058`)
  - `GL_KHR_texture_compression_astc_hdr`(`:201`), `_astc_sliced_3d`(`:237`), `GL_OES_texture_compression_astc`(`:649`)
  - `GL_EXT_texture_norm16`(`:2133`, 토큰 `GL_R16_EXT`·`GL_RG16_EXT`·`GL_RGBA16_EXT` `:2134–2136`), `GL_EXT_render_snorm`(`:1665`, `GL_R16_SNORM_EXT` 등 `:1669–1671`)
  - `GL_EXT_texture_format_BGRA8888`(`:2119`, `GL_BGRA8_EXT` `:1012`)
  - `GL_EXT_texture_sRGB_R8`(`:2150`, `GL_SR8_EXT` `:2151`), `GL_EXT_texture_sRGB_RG8`(`:2155`, `GL_SRG8_EXT` `:2156`)
  - `GL_OES_texture_float_linear`(`:690`), `GL_EXT_color_buffer_float`(`:1179`), `GL_EXT_color_buffer_half_float`(`:1183`)
- 각 확장 사양서의 세부(렌더 가능·필터 가능 여부, 불변 저장 `glTexStorage*`와의 조합)는 원문을 대조하지 않았다(**미확인**, 8.6절에 조건으로 적고 PF3 시험으로 검증).

**Metal(위치).**
- 텍스처 형식 표: MTL:798–1046(`GPixelFormats[...].PlatformFormat`), iOS·macOS 분기 MTL:830–977
- 버퍼 형식 표: MTL:700–796, `static_assert(PF_MAX == 94)` MTL:796
- 능력: MTL:1049–1062
- sRGB 변환: `UEToMetalFormat`(`Apple/MetalRHI/Private/MetalTexture.cpp:188–236`)

### 4.6 sRGB 처리

UE에서 sRGB는 형식이 아니라 텍스처 생성 플래그 `ETextureCreateFlags::SRGB`(`RHI/Public/RHIDefinitions.h:1100`, 별칭 `TexCreate_SRGB` `:1193`)다. 형식 하나에 플래그를 더해 backend가 sRGB 장치 형식을 고른다.

| backend | sRGB 장치 형식으로 바뀌는 지점 |
|---|---|
| D3D12·D3D11 | 자원은 typeless로 만들고 SRV·RTV 형식을 `FindShaderResourceFormat(format, bSRGB)`로 고른다(`DXGI:86–111`, `D3D12Texture.cpp:611–612, 1169, 1677–1680`). view마다 `bDisableSRGB`로 끌 수 있다(`RHIRes.h:2780`, 결정 `RHI/Private/RHIResources.cpp:415`). 짝이 없는 형식(R8 등)은 플래그가 있어도 선형 형식 그대로다 |
| Vulkan | 형식 표를 만든 뒤 sRGB 짝 표 `GVulkanSRGBFormat`을 만든다(VK:915–982). R8·RG8·RGB8·RGBA8·BGRA8·BC1/2/3/7·ETC2·ASTC LDR이 짝을 가진다. R8 sRGB는 ES3.1 기능 수준에서 쓰지 않는다(VK:923). 짝 형식을 샘플할 수 없으면 선형 형식으로 되돌린다(VK:956–963). sRGB 텍스처는 `MUTABLE_FORMAT`와 형식 목록(선형·sRGB)으로 만든다(`VulkanRHI/Private/VulkanTexture.cpp:387–399`) |
| OpenGL | 생성 때 `InternalFormat[1]`(GL 표의 sRGB 열)을 고른다(`OpenGLTexture.cpp:584`). 짝이 없으면 선형 형식이 그대로 들어 있다(예: `GL_RGBA32F` 두 칸, GL:1002) |
| Metal | `UEToMetalFormat(.., bSRGB)`가 RGBA8·BGRA8·BC1/2/3/7(macOS)·R8·ASTC(Apple GPU)를 `_sRGB` 형식으로 바꾼다(`MetalTexture.cpp:193–233`). ETC2 sRGB는 대응이 없다 |

쿠킹 쪽에서는 회색 8비트 + sRGB를 Windows에서 BGRA8로 대체한다(`Engine/Private/Texture.cpp:4438–4446`, Windows는 `SupportsGrayscaleSRGB() = false` `Core/Public/Windows/WindowsPlatformProperties.h:96–99`). R8 sRGB 장치 형식이 D3D에 없기 때문이다.

### 4.7 깊이·스텐실

| UE 형식 | 의미 | D3D12 | Vulkan | OpenGL | Metal |
|---|---|---|---|---|---|
| `PF_DepthStencil` | 플랫폼이 고르는 깊이 + 스텐실(`NoExportTypes.h:221–222`) | D32FS8 기본, D24S8은 설정(D12S:870–889) | D32FS8 우선 기본, D24S8은 설정(VK:832–845) | D24S8 기본(GL:1004–1015) | D24S8 가능하면, 아니면 D32FS8(MTL:809–828) |
| `PF_ShadowDepth` | 플랫폼이 고르는 그림자 깊이(`NoExportTypes.h:223–224`) | D16(D12S:890) | D16(VK:770) | D16(GL:1017, `OpenGLES.h:70`) | D16 또는 D32F(MTL:947–955) |
| `PF_D24` | 24비트 깊이, 스텐실 없음 | R24G8 typeless(D12S:900) | X8_D24 우선 대체 사슬(VK:781) | ES D24, 데스크톱 기본 D16(`OpenGLES.h:69`, `OpenGLBase.h:117`) | D24S8 또는 D32F(MTL:958–965) |
| `PF_R32_FLOAT` + 깊이 타깃 | D32F | `R32_TYPELESS` → `D32_FLOAT`(`DXGI:126, 172`) | 없음(`PF_D24` 사슬이 D32F를 포함) | 없음 | 없음 |
| `PF_X24_G8` | 스텐실 읽기 view 형식 | `X24_TYPELESS_G8_UINT`·`X32_TYPELESS_G8X24_UINT`, plane 1(D12S:876, 886, `DXGI:217–236`) | D/S VkFormat + stencil aspect, RR01(VK:836, `VulkanRHI/Private/VulkanView.cpp:192–199`) | `GL_DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX` + swizzle (0, R, 0, 0)(`OpenGLUAV.cpp:127–139`) | `X24_Stencil8`·`X32_Stencil8`(MTL:815, 823) |

스텐실 읽기 view는 plane 선택으로 처리된다. view desc의 plane `ERHITexturePlane::Stencil`(`RHIRes.h:2639–2662`)을 고르면 형식이 `PF_X24_G8`로 정해지고, 반대로 `PF_X24_G8`를 주면 plane이 스텐실로 바뀐다(`RHI/Private/RHIResources.cpp:348–372`). 셰이더는 스텐실 값을 G 채널에서 읽는다(D3D `X24_TYPELESS_G8_UINT`의 G, GL swizzle의 G). 깊이·스텐실과 `PF_D24` 텍스처의 subresource plane 수는 2다(`RHIRes.h:1939`).

### 4.8 비디오 형식과 다중 plane

- `PF_NV12`: 휘도 R8 plane + ¼ 크기 색차 R8G8 plane(`PF.cpp:110–112`). `PF_P010`: R16 + R16G16, 상위 10비트 사용(`PF.cpp:137–139`). 블록 정보는 "의미 없음"으로 1을 넣었다.
- D3D12·D3D11만 대응한다(D12S:938–939, 947–948, D11:177–178, 187–188). Vulkan·OpenGL은 대응이 없고 Metal은 Invalid다(MTL:1024).
- D3D에서는 plane별 view 형식으로 plane을 고른다. NV12는 R8 → 0, R8G8 → 1, P010은 R16 → 0, R16G16 → 1이다(`DXGI:238–251`). 폭·높이는 2의 배수여야 한다(`DXGI:277–278, 315–316`). 단 `GetPlaneCount`는 깊이·스텐실만 2로 센다(`DXGI:258–269`).
- 텍스처 생성 때 SRV·RTV를 생성하지 않는다(`D3D12Texture.cpp:1704–1709`). 4 KiB 정렬 대상에서도 제외한다(`:187–191`).

### 4.9 압축 형식의 쿠킹 대상(위치)

쿠킹 자체는 에셋 쪽 일이므로 위치만 기재한다.
- 공통 기본 형식 이름 결정: `GetDefaultTextureFormatNamePerLayer`(`Engine/Private/Texture.cpp:4580`, 선언 `Engine/Public/TextureResource.h:690`), 설정별 기본 이름 `GetDefaultTextureFormatName`(`Texture.cpp:4201`, AssetSystem 12.4.3절이 인용).
- Windows: `Developer/Windows/WindowsTargetPlatfomControls/Public/GenericWindowsTargetPlatformControls.h:172–178`. 공통 기본값을 그대로 쓴다(BC 계열).
- Android: `Developer/Android/AndroidTargetPlatformControls/Private/AndroidTargetPlatformControls.cpp`
  - 공통 `:487–540`, ASTC `:542–602`(볼륨 텍스처는 ETC2로 되돌림 `:568–586`, ASTC HDR 미지원 시 대체 `:590–598`)
  - DXT `:604–626`(BC6H → RGBA16F, BC7 → DXT5)
  - ETC2 `:628–645`, 다중 `:647–`
- 압축 모듈: `Developer/TextureFormatDXT`(BC1–5), `TextureFormatIntelISPCTexComp`, `TextureFormatOodle`, `TextureFormatASTC`, `TextureFormatETC2`, `TextureFormatUncompressed`. BC6H·BC7을 어느 모듈이 내는지는 이 조사에서 확인하지 않았다(**미확인**).
