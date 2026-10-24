Bundled: Figtree (regular.ttf, bold.ttf) and Noto Sans CJK (fallback.otf), both SIL OFL.
To use other fonts, replace these files before building the VPK:

  regular.ttf    Latin UI font, regular weight   (e.g. Figtree-Regular, Inter-Regular)
  bold.ttf       Same family, bold weight         (e.g. Figtree-Bold, Inter-Bold)
  fallback.ttf   CJK coverage, optional           (e.g. Noto Sans TC/JP; .otf/.ttc also work)
  fallback2.ttf  Second fallback, optional         (e.g. Noto Sans KR, for Hangul)

A single pan-CJK font (Noto Sans CJK) as fallback.ttf covers all three scripts.

The bundled fallback.otf is a subset of Noto Sans CJK TC Regular 2.004
(NotoSansCJKtc-Regular.otf, 16.4 MB) cut to 6.5 MB, because the app reads the
whole file into memory before it draws any CJK text (about 2 s on the Vita for
the full font). It keeps every BMP character except CJK Extension A
(U+3400-4DBF) and drops characters above U+FFFF, which text.c cannot draw
anyway, plus the OpenType layout features (regional and vertical forms) and
hinting, which the app doesn't use. Rebuild it with fonttools
(pip install fonttools):

  pyftsubset NotoSansCJKtc-Regular.otf --unicodes="U+0000-33FF,U+4DC0-FFFF" \
    --layout-features="" --no-hinting --output-file=fallback.otf

Use fonts whose licence allows redistribution (SIL OFL fonts from Google Fonts are fine).
Do not use Spotify's proprietary typeface.

At runtime the app also looks in ux0:data/VitaSpot/fonts/ first, so users can swap
fonts without rebuilding. On desktop, missing files fall back to system fonts
(Segoe UI, Microsoft JhengHei and Malgun Gothic on Windows).
