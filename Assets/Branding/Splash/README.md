# Splash artwork

Studio and engine fallback splashes share the obsidian artwork and the white stacked Rocks logo on the left. Foundation projects use the same artwork with a small white symbol in the lower right. The runtime draws the project title, version, and progress in the bottom 96 logical pixels.

| Generator argument | Output directory | Filename |
| --- | --- | --- |
| `studio` | `Engine/Content/Splash` | `StudioSplash.png`, `StudioSplash@2x.png` |
| `game` | `Engine/Content/Splash` | `Splash.png`, `Splash@2x.png` |
| `template` | `Templates/Game/Foundation/Content/Splash` | `Splash.png`, `Splash@2x.png` |

`Obsidian.png` (1774 × 887, no logo) is generative AI artwork (ChatGPT) supplied by the project owner on 2026-11-27. The design rules for the images are in [Docs/Design/SplashScreen](../../../Docs/Design/SplashScreen/12-DefaultImages.ko.md) section 12.

Run `python make_splash.py <repo root> <output directory> <studio|game|template>` with Python 3 and Pillow. The generator resizes the logo-free `Obsidian.png` and composites the official white PNG from `../PNG` at each target size (768 × 384 and 1536 × 768).

Run `python mockup.py <image> <output> <scale> <title> <version> <progress text> <fraction> <copyright>` to preview the runtime text layout on Windows.
