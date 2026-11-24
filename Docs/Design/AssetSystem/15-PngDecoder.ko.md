## 15. PNG 디코더 범위

색 형식 0/2/3/4/6, 비트 깊이 1–16, Adam7, PLTE·tRNS, chunk CRC, zlib(Core `Compression`)·Adler-32, 한 변 16384·해제 크기 = IHDR 계산값. gAMA·cHRM·sRGB·iCCP는 변환하지 않고 sRGB로 보며 sRGB가 아닌 표시가 있으면 `NonSrgbSource` 경고(판정 수치와 우선순위는 12.9절). APNG는 기본 이미지. 시험은 PngSuite(자유 사용 라이선스, `ImageCore/Tests/Fixtures/PngSuite`).
