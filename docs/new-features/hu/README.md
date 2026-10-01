[Magyar](README.md) | [English](../en/README.md)

## Funkciók mindenkinek

> Azon funkciók és fejlesztések listája, mely nem csak formázók számára lehetnek érdekesek. Néhány funkció külön oldalon van részletezve. 

### Videók, hozzárendelt fájlok
- **<ins>MKV betöltés</ins>**: a fájl menüben betölthető az MKV videó egyben felirattal, videóval egy kattintással
- **<ins>Gyors videóbetöltés</ins>**: `video/open/recent` parancs a legutóbbi videó betöltéséhez
- **<ins>Utólagos betöltés</ins>**: a felirat menüből elérhető az aktuális fájlhoz tartozó videó/audió betöltése utólag
- [Forrásmappák](source-folders.md): videók kényelmesebb betöltéséhez

### Munkafolyamat
- [Forrássorok](source-lines.md): forrás mondat (pl. angol) automatikus megjegyzése
- [AI review és utóellenőrzés](ai-review.md): jelenet fordításának ellenőrzése japánból hang alapján, utólagos helyesírási, stilisztikai, fordítási ellenőrzés
- [Keresés mappában](find-in-folder.md): adott kifejezésre való kényelmes keresés több feliratban egyszerre
- **<ins>Szöveg módosítás</ins>**: kijelölt sorokra jobb klikk menüben `Szöveg módosítása` vagy `edit/line/change-text` paranccsal több sor szövegét lehet egyszerre módosítani és a karakterenkénti gradientet is újraalkalmazza arányosan
- **<ins>Szöveg megjegyzésbe rakása</ins>**: kijelölt szövegrész az editorban jobb klikk menüben `Megjegyzés` vagy `edit/comment` paranccsal {} karakterek közé rakható
- **<ins>Szöveg másolása a fordítónak</ins>**: jobb klikk menüben `Szöveg másolása a fordítónak` vagy `grid/copytext/translator` paranccsal adott sort lehet másolni és ilyen formában formázás és egyéb karakterek nélkül ki lehet másolni a sort: `01:42 --- "Nézze meg ezt."`

### Felület
- [Fontválasztó](font-picker.md): teljesen újradolgozott fontválasztó, rengeteg extrával
- [Dark mód](dark-mode.md): rendes dark mód ikonokkal
- **<ins>Fényerő és lejátszási sebesség</ins>**: a videó fényerejét 0-400% között, a lejátszási sebességet pedig 0.25 - 10x között lehet beállítani (jobb klikkel resetelni), és a hang is igazodik hozzá. Támogatott audio playerek: DirectSound, PulseAudio és ALSA

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