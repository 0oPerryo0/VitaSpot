Bundled: Figtree (regular.ttf, bold.ttf) and Noto Sans CJK (fallback.otf), both SIL OFL.
To use other fonts, replace these files before building the VPK:

  regular.ttf    Latin UI font, regular weight   (e.g. Figtree-Regular, Inter-Regular)
  bold.ttf       Same family, bold weight         (e.g. Figtree-Bold, Inter-Bold)
  fallback.ttf   CJK coverage, optional           (e.g. Noto Sans TC/JP; .otf/.ttc also work)
  fallback2.ttf  Second fallback, optional         (e.g. Noto Sans KR, for Hangul)

A single pan-CJK font (Noto Sans CJK) as fallback.ttf covers all three scripts.

Use fonts whose licence allows redistribution (SIL OFL fonts from Google Fonts are fine).
Do not use Spotify's proprietary typeface.

At runtime the app also looks in ux0:data/VitaSpot/fonts/ first, so users can swap
fonts without rebuilding. On desktop, missing files fall back to system fonts
(Segoe UI, Microsoft JhengHei and Malgun Gothic on Windows).
