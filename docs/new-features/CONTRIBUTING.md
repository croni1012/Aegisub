# A dokumentáció szerkesztése / Editing the documentation

[Magyar](hu/README.md) | [English](en/README.md)

## Magyar

- Csak a kibővített útmutatóban szereplő új vagy továbbfejlesztett funkciók tartoznak ide. Az általános telepítés és a tervezett funkciók nem részei ennek a kézikönyvnek.
- A helyi import forrása a `C:\aegisub-portable\aegisub-docs` mappa, az oldal által betöltött `content.js` és `generated-translations.js` együttes tartalmával (az oldal verziója: `2026-08-26-09`). Az import után a Markdown-fájlokat közvetlenül szerkesztjük; nincs szükség weboldalgenerátorra.
- Egy téma ugyanazon a fájlnéven szerepel a `hu` és `en` mappában. A nyelvváltó mindig ugyanarra a témára mutasson.
- Új oldalnál frissítsd mindkét nyelv kezdőlapját és a kapcsolódó funkciók linkjeit. A menühelyeket és parancsazonosítókat a program forrásából ellenőrizd; ne következtess automatikusan a menühelyből minden platform támogatására.
- Képek: `![Leíró képszöveg](../media/font-picker.png)`. Videók: `[MP4 megnyitása / letöltése](../media/color-picker.mp4?raw=true)`. A videólink megnyitást vagy letöltést biztosít, nem ígér beágyazott GitHub-lejátszót.
- Az eredeti médiák egyszer, változatlan fájlnéven szerepelnek a `media` mappában. A meglévő `tpyesetting-image-insert-and-edit.mp4` elírást a forrásazonosság miatt megtartottuk.
- Ellenőrizd a relatív linkeket, a képek létezését és a GitHub Preview nézetét. Mindent UTF-8-ként ments. A szolgáltatói díjak és keretek változhatnak, ezért ne másolj át dátum nélküli ár- vagy kvótaígéreteket.

## English

- Scope is limited to new or enhanced features covered by the original extended guide. General installation instructions and planned features are excluded.
- The initial import comes from `C:\aegisub-portable\aegisub-docs`, combining the page's `content.js` and `generated-translations.js` (page version `2026-08-26-09`). Edit the Markdown directly after import; no site generator is required.
- Use identical filenames under `hu` and `en`, and link each page to its translation. Update both indexes and related-feature links when adding a topic.
- Verify menu locations and command identifiers against the application source. Do not assume a menu or setting is available on every platform.
- Images use relative Markdown image links; videos use ordinary links to the MP4 with `?raw=true`. This provides an open/download link, not a promise of inline playback on GitHub.
- Shared media keeps the original filenames, including the existing `tpyesetting-image-insert-and-edit.mp4` spelling. Avoid duplicate copies per language unless the screenshot itself differs.
- Check relative links, image files, UTF-8 encoding, and GitHub Preview. Provider prices and quotas should not be copied as undated guarantees.
