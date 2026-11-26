# Splash image generator

Generates the default Rocks Engine splash images. The design and the text slots the engine draws over the images are specified in [Docs/Design/SplashScreen](../../../Docs/Design/SplashScreen/README.ko.md) (sections 5, 6 and 12).

| Output | Use |
| --- | --- |
| `Engine/Content/Splash/StudioSplash.png`, `StudioSplash@2x.png` | Rocks Studio |
| `Engine/Content/Splash/Splash.png`, `Splash@2x.png` | Games and engine programs without their own splash |
| `Templates/Game/Foundation/Content/Splash/Splash.png`, `Splash@2x.png` | Placeholder splash of new projects |

All images are 768 × 384 logical pixels (2:1); `@2x` is 1536 × 768. The template art is drawn at a height of 432 and its top 48 rows of sky are cropped, which keeps the scene composition. The bottom 96 logical pixels are the text band the engine draws into, so it is kept dark and calm.

## Brand rules

The logos are the white PNG exports in `../PNG`, scaled once to the final size and never reshaped (Guidelines sections 2, 6, 8 and 9). The stacked lockup is used for the Studio splash as the guidelines assign it to splash screens. Where the horizontal lockup would be narrower than its 240 px minimum, only the symbol is used. Background shading never touches the logo artwork.

## Regenerating

Requires Python 3 with numpy and Pillow. The noise and terrain use fixed seeds, so the output is reproducible.

```text
python make_splash.py <repo root> <out dir> studio|game|template
python mockup.py <image> <out.png> <scale> <title> <version> <progress text> <fraction> <copyright>
```

`make_splash.py studio` and `game` write `StudioSplash*.png` and `Splash*.png` for `Engine/Content/Splash`; `template` writes `Splash*.png` for the Foundation template. `mockup.py` composes the default text layout (Segoe UI) over an image for review; it is not the engine renderer. Commit the generator together with any regenerated images.
