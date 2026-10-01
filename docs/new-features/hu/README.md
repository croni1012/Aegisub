[Magyar](README.md) | [English](../en/README.md)

## Funkciók mindenkinek

> Azon funkciók és fejlesztések listája, mely nem csak formázók számára lehetnek érdekesek. Néhány funkció külön oldalon van részletezve. 

### Videók, hozzárendelt fájlok
- MKV betöltés: a fájl menüben betölthető az MKV videó egyben felirattal, videóval egy kattintással
- `video/open/recent` parancs a legutóbbi videó betöltéséhez
- Utólagos betöltés: a felirat menüből elérhető az aktuális fájlhoz tartozó videó/audió betöltése utólag
- [Forrásmappák](source-folders.md): videók kényelmesebb betöltéséhez

### Munkafolyamat
- [Forrássorok](source-lines.md): forrás mondat (pl. angol) automatikus megjegyzése
- [AI review és utóellenőrzés](ai-review.md): jelenet fordításának ellenőrzése japánból hang alapján, utólagos helyesírási, stilisztikai, fordítási ellenőrzés
- [Keresés mappában](find-in-folder.md): adott kifejezésre való kényelmes keresés több feliratban egyszerre
- Szöveg módosítás: kijelölt sorokra jobb klikk menüben `Szöveg módosítása` vagy `edit/line/change-text` paranccsal több sor szövegét lehet egyszerre módosítani és a karakterenkénti gradientet is újraalkalmazza arányosan
- Szöveg megjegyzésbe rakása: kijelölt szövegrész az editorban jobb klikk menüben `Megjegyzés` vagy `edit/comment` paranccsal {} karakterek közé rakható
- Szöveg másolása a fordítónak: jobb klikk menüben `Szöveg másolása a fordítónak` vagy `grid/copytext/translator` paranccsal adott sort lehet másolni és ilyen formában formázás és egyéb karakterek nélkül ki lehet másolni a sort: `01:42 --- "Nézze meg ezt."`

### Felület
- [Dark mód](dark-mode.md): rendes dark mód ikonokkal
- [Fényerő és lejátszás](playback.md): videó fényerejének és lejátszási sebessége
- [Fontválasztó](font-picker.md): teljesen újradolgozott fontválasztó, rengeteg extrával

### Egyéb
- Sorok eltolása az aktuális képkockához a végidejük alapján (Időzítés menü vagy `time/frame/current_end` parancs)

## Formázóknak

> Kifejezetten formázók számára fejlesztett kényelmi funkciók, hogy egy rész elkészítése könnyebb legyen, adott esetben akár jóval hatékonyabb is. Néhány funkció ezelőtt akár elérhetetlen is volt a kezdő formázók számára, mert akár programozói ismeretek voltak szükségesek hozzá vagy a scriptek teljeskörű ismerete és egy kis kreativitás (pl. glitch effekt, szövegdoboz, szekresztéses képbeillesztés)

- [Sorok elrejtése](hide-subtitles.md)
- [Színkijelölés](color-picker.md)
- [Transzformáció](transformations.md)
- [Clippelés](vector-clip.md)
- [Pipetta mód](clip-eyedropper.md)
- [Maszkok](masks.md)
- [Alakzatok rajzolása](shapes.md)
- [Gradient](gradient.md)
- [Szövegdoboz](text-box.md)
- [Glitch effekt](glitch.md)
- [Motion és Auto motion](motion.md)
- [Képbeillesztés szerkesztéssel](image-editor.md)
- [Vizuális eszközök](visual-tools.md)
- [Képmaszkok és foldok](folds.md)
- [Utolsó script](last-script.md)

## Apróbb javítások

> Olyan apróbb javítások listája, melyek már meglévő dolgon javítanak kicsit és nem fért máshova.

- 32-nél több fájlt tartalmazó MKV-k megnyitása
- Videó doboz villogása méretezés közben