# Bundled editor fonts

These open-source fonts are embedded (DEFLATE-compressed) into the RapidEditor
executable and offered in **视图 → 编辑器字体**. All are licensed under the
SIL Open Font License 1.1, which permits embedding and redistribution; the full
license texts are included here next to the font files.

| Font file | Family | Upstream | License |
| --- | --- | --- | --- |
| `JetBrainsMono-Regular.ttf` | JetBrains Mono | github.com/JetBrains/JetBrainsMono | `LICENSE-JetBrainsMono.txt` |
| `CascadiaCode-Regular.ttf` | Cascadia Code | github.com/microsoft/cascadia-code | `LICENSE-Cascadia.txt` |
| `CascadiaMono-Regular.ttf` | Cascadia Mono | github.com/microsoft/cascadia-code | `LICENSE-Cascadia.txt` |
| `SourceCodePro-Regular.ttf` | Source Code Pro | github.com/adobe-fonts/source-code-pro | `LICENSE-SourceCodePro.md` |
| `IBMPlexMono-Regular.ttf` | IBM Plex Mono | github.com/IBM/plex | `LICENSE-IBMPlex.txt` |

All files are TrueType (`glyf`) outlines so they render through CUI's stb_truetype
rasterizer. `CascadiaMono` was copied from the Windows system font directory
(`C:\Windows\Fonts`); the rest were downloaded from their upstream repositories.

## Regenerating the embedded data

After adding or replacing a font here:

1. Update the `FONTS` list in `scripts/make_font_bundle.py`.
2. Run `python3 scripts/make_font_bundle.py` to regenerate
   `src/np_font_bundle.c` / `.h`.
3. Rebuild the app (the new source files are compiled automatically).

The Chinese glyphs are not provided by these Latin fonts; a system CJK face
(微软雅黑 / 苹方 / Noto Sans CJK / 文泉驿) is attached as a fallback at runtime.
