# ROCKS ENGINE — Visual Identity Guidelines

**Version 0.1 · 14 November 2026 · Core / C2 working identity**

This package develops the selected Core direction into editable SVG artwork. The mark is a manually constructed vector interpretation of the C2 concept, with custom geometric lettering. These guidelines define the current working identity; they do not record final brand approval.

Open [Guidelines.html](Guidelines.html) for the visual reference. All logo artwork has a transparent background and contains vector paths, with no embedded images, external resources, or font dependencies.

## 1. Identity

**Brand name:** Rocks Engine. Use **ROCKS ENGINE** in display treatments and **Rocks Engine** in running text. Use `rocks` only where required by code or technical identifiers.

The Core symbol combines three substantial segments into a compact circular silhouette. Its curved outer edges suggest controlled motion; its angular central opening gives the mark a cut, solid character. Preserve the three-part construction and its orientation.

## 2. Logo system

| Asset | Intended use | SVG canvas | Aspect ratio |
| --- | --- | --- | --- |
| [Symbol](Logos/rocks-symbol.svg) | Editor icon, avatar, compact brand signature | 448 × 448 | 1:1 |
| [Stacked logo](Logos/rocks-logo-stacked.svg) | Splash screen, cover, centered signature | 448 × 638 | 224:319 |
| [Horizontal logo](Logos/rocks-logo-horizontal.svg) | Website header, documentation, footer | 688 × 192 | 43:12 |

Each asset has a `-white.svg` counterpart for dark backgrounds. Use the supplied lockups as complete units. Do not reset the wordmark, move individual letters, or change the relationship between the symbol and lettering.

### Construction

| Measurement | Stacked master | Horizontal master |
| --- | --- | --- |
| Nominal symbol height, H | 448 units | 192 units |
| Symbol-to-wordmark gap | 56 units vertically | 48 units horizontally |
| ROCKS wordmark width | 448 units | 448 units |
| ROCKS letter construction | Five outlined glyphs on 112-unit origins, uniformly scaled | Same lettering and scale |
| ENGINE nominal cap height | 24 units | 24 units |
| ENGINE glyph origin spacing | 58 units | 58 units |
| Wordmark-to-descriptor gap | Approximately 24 units | Approximately 24 units |

The SVG files are the authoritative geometric masters. Measurements describe those files; they are not instructions to recreate the logo with live text. Small optical overshoots in curved letters are intentional.

## 3. Color palette

Digital color values use **sRGB**. The core identity is monochrome. No accent color is assigned in this version.

| Color | HEX | RGB | Role |
| --- | --- | --- | --- |
| Rocks Black | `#111111` | 17, 17, 17 | Primary logo, headings, dark brand surfaces |
| Pure White | `#FFFFFF` | 255, 255, 255 | Reversed logo, neutral background |
| Warm Ivory | `#F5F2EB` | 245, 242, 235 | Supporting presentation background |
| Slate | `#666666` | 102, 102, 102 | Secondary text; not a logo color |
| Rule Gray | `#D8D5CE` | 216, 213, 206 | Dividers and diagram rules; not a logo color |

### Print reproduction

For one-color black printing, use **C0 M0 Y0 K100**. For reversed artwork, use the unprinted paper surface or a production-specific white ink. This print instruction is a deliberate single-ink treatment, not an exact conversion of `#111111`.

No Pantone reference or universal CMYK match is assigned. Convert supporting colors using the printer's destination ICC profile and approve a physical proof when color matching matters.

## 4. Typography

### Logo lettering

**ROCKS** and **ENGINE** are custom outlined lettering in the supplied SVGs. They have no corresponding installable font or editable text weight. Use the artwork rather than typing the brand signature. The supporting fonts below are for communication and interface text.

### Supporting type system

| Use | Typeface | Weight | Tracking | Line height |
| --- | --- | --- | --- | --- |
| Latin display heading | Montserrat ExtraBold | 800 | −0.02em | 1.10 |
| Latin section heading | Montserrat SemiBold | 600 | −0.01em | 1.25 |
| Korean heading | Pretendard Bold | 700 | −0.02em | 1.30 |
| Body text, Latin or Korean | Pretendard Regular | 400 | 0 | 1.60 |
| UI label / button | Pretendard SemiBold | 600 | 0 | 1.40 |
| Small uppercase eyebrow | Montserrat SemiBold | 600 | +0.12em | 1.40 |

Suggested website sizes: display **48–72 px** on desktop and **32–40 px** on mobile; section headings **24–32 px**; body **16 px**; labels **14 px**. These are layout defaults, not fixed logo dimensions.

```css
/* Load the designated fonts in the consuming application. */
:root {
    --rocks-black: #111111;
    --rocks-white: #ffffff;
    --rocks-ivory: #f5f2eb;
    --rocks-slate: #666666;
    --rocks-rule: #d8d5ce;
    --rocks-font-display: "Montserrat", "Arial", sans-serif;
    --rocks-font-body: "Pretendard Variable", "Pretendard", -apple-system,
        BlinkMacSystemFont, "Segoe UI", "Malgun Gothic", sans-serif;
}
```

**Font sources:** [Montserrat on Google Fonts](https://fonts.google.com/specimen/Montserrat) · [Montserrat source and license](https://github.com/google/fonts/tree/main/ofl/montserrat) · [Pretendard source and license](https://github.com/orioncactus/pretendard).

Both supporting font families are distributed under SIL Open Font License 1.1. Font binaries are not included in this package. Obtain them from the official sources and retain their license notices when distributing font files. The local visual guide uses installed fonts with system fallbacks and is not a proof of the designated font files.

## 5. Clear space

Let **H** be the nominal height of the symbol in the chosen master. Define **x = H ÷ 8**.

Keep at least **x** clear on all four sides of the entire logo's visible artwork. Measure from the outermost visible edge, including the ENGINE descriptor. Keep text, borders, imagery, and container edges outside this area. Clear space is added outside the SVG canvas; the supplied canvases do not include a full protective margin.

| Master | H | Minimum clear space, x |
| --- | --- | --- |
| Symbol | 448 units | 56 units |
| Stacked logo | 448 units | 56 units |
| Horizontal logo | 192 units | 24 units |

Scale H and x together. For example, a horizontal SVG displayed at **344 × 96 px** requires **12 px** of clear space on each side. A symbol displayed at **32 × 32 px** requires **4 px** around it.

## 6. Minimum size

These are initial production recommendations for this working identity. Check the actual output on the target display or print process.

| Asset | Digital minimum, SVG display width | Print minimum, artwork width |
| --- | --- | --- |
| Symbol | 24 px | 8 mm |
| Stacked logo | 160 px | 30 mm |
| Horizontal logo | 240 px | 45 mm |

At sizes below the lockup minimum, use the symbol alone. A 16 px favicon is an exception that requires a separately optimized raster export; it is not included in this package. Do not shrink the full logo into a favicon.

## 7. Backgrounds and contrast

| Background | Logo treatment |
| --- | --- |
| White or Warm Ivory | Rocks Black |
| Rocks Black or a sufficiently dark, even surface | Pure White |
| Photography or rendered scenes | Place on a quiet region with clear separation, or use a solid brand panel with clear space |
| Mid-tone, detailed, or changing imagery | Use a solid White, Ivory, or Black panel |

Use **3:1** as an internal minimum contrast target between the symbol and a flat background. This is a brand legibility target, not a claim that logos have a WCAG contrast requirement. Text in the surrounding interface needs its own accessibility review.

Do not apply an automatic dark-mode filter to the black SVG. Select the supplied white file explicitly.

## 8. Incorrect usage

- Do not stretch, skew, rotate, mirror, or crop the artwork.
- Do not alter the central opening or separate, rearrange, or animate individual segments in the static signature.
- Do not use gradients, multicolor segments, outlines, bevels, shadows, or glow as the master logo.
- Do not add a surrounding ring, shield, or decorative container to the symbol.
- Do not replace the custom lettering with typed text.
- Do not change symbol-to-wordmark proportions or descriptor spacing.
- Do not place the logo over detail that obscures its silhouette.
- Do not introduce an accent color into the logo without a deliberate identity revision.

Motion treatments and splash-screen animation may be developed separately. Their final resting state should match the static master.

## 9. File use and export

Use SVG for websites, documentation, and vector design tools. The files contain no scripts or external references. Use the black or white asset appropriate to the background. When inlining several SVGs in one page, give their accessibility IDs unique prefixes.

For engine textures, app icons, or consumers without SVG support, use the included PNG exports or export at the required pixel size with alpha transparency. The PNGs are rasterized from the supplied SVG paths; they are not the original concept images. Export raster artwork at the intended final dimensions; do not rely on repeated resizing. Package ICO or platform-specific icon formats only after checking the required size variants.

| Included PNG pair | Dimensions | Background |
| --- | --- | --- |
| `PNG/rocks-symbol.png`, `PNG/rocks-symbol-white.png` | 1024 × 1024 px | Transparent |
| `PNG/rocks-logo-stacked.png`, `PNG/rocks-logo-stacked-white.png` | 896 × 1276 px | Transparent |
| `PNG/rocks-logo-horizontal.png`, `PNG/rocks-logo-horizontal-white.png` | 1376 × 384 px | Transparent |

The white exports contain white artwork on transparency and may look blank in a white preview. Inspect them on a dark background. PNG dimensions exclude the additional clear space required by the surrounding layout.

Use the default SVG aspect ratio. Apply clear space in the surrounding layout. Keep the six master files together so all applications share the same geometry.

## 10. Package contents

- `Logos/rocks-symbol.svg` — black symbol.
- `Logos/rocks-symbol-white.svg` — white symbol.
- `Logos/rocks-logo-stacked.svg` — black stacked lockup.
- `Logos/rocks-logo-stacked-white.svg` — white stacked lockup.
- `Logos/rocks-logo-horizontal.svg` — black horizontal lockup.
- `Logos/rocks-logo-horizontal-white.svg` — white horizontal lockup.
- `PNG/` — six matching transparent PNG exports, in the dimensions listed above.
- `Guidelines.html` — local visual guide; open directly in a browser.
- `README.md` — complete written specification.
