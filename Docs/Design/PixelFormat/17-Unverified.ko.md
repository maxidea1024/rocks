## 17. 미확인 목록

| 항목 | 확인 시점 |
|---|---|
| ES 확장(`EXT_texture_norm16`·`EXT_texture_sRGB_R8/RG8`·`EXT_texture_format_BGRA8888`·`OES_texture_float_linear`의 대상 형식·`KHR_texture_compression_astc_sliced_3d`)의 렌더 가능·필터 가능·불변 저장 조합 세부 | PF2 착수 때 확장 사양서 대조, PF3 시험으로 검증 |
| `glGetInternalformativ`·버퍼 텍스처(Table 8.18)·image atomic 대상 형식의 사양 절 | PF3 |
| 데스크톱 GL context가 ES 확장 이름(s3tc·rgtc·bptc)을 노출하는 방식 | PF2 |
| UE ES의 `PF_B5G5R5A1_UNORM` 대응(5_5_5_1 + swizzle) 비트 배치 정확성 | Rocks는 미지원으로 두므로 필요 시 |
| Vulkan 필수 형식 표와의 대조, `R64Uint`에 필요한 장치 기능 | SH2(PF4) |
| Metal 대응 전반(8.7절) | SH5(PF5) |
| BC6H·BC7을 내는 UE 압축 모듈 | 압축 단계(에셋 쪽) |
