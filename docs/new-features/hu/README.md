[Magyar](README.md) | [English](../en/README.md)

# Aegisub -- nyaa's edition

A projekt azért készült, hogy megkönnyítse a fansubbolást vagy akár olyan lehetőségeket biztosítson mindenki számára, amire addig nem volt igazán lehetőség vagy csak nagyon macerás kerülőúttal. Több fansubberrel egyűttműködve lettek megalkotva a funkciók, de aktív fejlesztés alatt áll, így érdemes figyelemmel követni.

> Innen beszerezheted: [https://github.com/croni1012/Aegisub/releases](https://github.com/croni1012/Aegisub/releases)

> Hibát találtál vagy ötleted volna? Írj egy issuet vagy bátran kereshetsz Discordon: [nyaa](https://discord.com/users/209700198350323715)

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
- **<ins>Kulcskocka beolvasás és időzítés javítás</ins>**: beépített funkció a kulcskockák beolvasásához ([bővebb infó](https://github.com/eldonishere/scuisei-rs)) a `Videó → Kulcskockák betöltése a videóból` menüpontban és kijelölt sorok időzítését lehet intelligensen javítani az `Időzítés → Kijelölt sorok intelligens javítása...` menüpontban (folytonosság, kulcskockához igazítás, stb -- **javasolt a munkamenet legelején futtatni**)
- **<ins>Szöveg módosítás</ins>**: kijelölt sorokra jobb klikk menüben `Szöveg módosítása` vagy `edit/line/change-text` paranccsal több sor szövegét lehet egyszerre módosítani és a karakterenkénti gradientet is újraalkalmazza arányosan
- **<ins>Szöveg megjegyzésbe rakása</ins>**: kijelölt szövegrész az editorban jobb klikk menüben `Megjegyzés` vagy `edit/comment` paranccsal {} karakterek közé rakható
- **<ins>Szöveg másolása a fordítónak</ins>**: jobb klikk menüben `Szöveg másolása a fordítónak` vagy `grid/copytext/translator` paranccsal adott sort lehet másolni és ilyen formában formázás és egyéb karakterek nélkül ki lehet másolni a sort: `01:42 --- "Nézze meg ezt."`

### Felület
- [Fontválasztó](font-picker.md): teljesen újradolgozott fontválasztó, rengeteg extrával
- [Dark mód](dark-mode.md): rendes dark mód ikonokkal
- **<ins>Fényerő és lejátszási sebesség</ins>**: a videó fényerejét 0-400% között, a lejátszási sebességet pedig 0.25 - 10x között lehet beállítani a videódoboz alatt (jobb klikkel resetelni), és a hang is igazodik hozzá. Támogatott audio playerek: DirectSound, PulseAudio és ALSA

### Egyéb
- Sorok eltolása az aktuális képkockához a végidejük alapján (`Időzítés` menü vagy `time/frame/current_end` parancs)

## Formázóknak

> Kifejezetten formázók számára fejlesztett kényelmi funkciók, hogy egy rész elkészítése könnyebb legyen, adott esetben akár jóval hatékonyabb is. Néhány funkció ezelőtt akár elérhetetlen is volt a kezdő formázók számára, mert akár programozói ismeretek voltak szükségesek hozzá vagy a scriptek teljeskörű ismerete és egy kis kreativitás (pl. glitch effekt, szövegdoboz, szekresztéses képbeillesztés). Néhány funkció külön oldalon van részletezve.

- **<ins>Sorok elrejtése</ins>**: videón megjelenő összes sor vagy csak maszkok elrejtése, áttetszőség állítása csak vízuálisan (pl. nem megjegyzésbe kerülnek a sorok) a videódoboz alatt, videóra kattintott jobbklikk menüben vagy `video/toggle_mask` és `video/toggle_subtitle` paranccsokkal
- **<ins>Utolsó script</ins>**: korábbi script újrafuttatása az `Automatizáció → Utolsó script` menüponttal vagy `am/last` paranccsal, a hozzárendelt billentyű gyors kétszeri megnyomása listát nyit az utoljára használt scriptlehetőségekből
- **<ins>Képmaszkok és foldok</ins>**: A beillesztett képek összevonva jelennek meg, melyet az egér görgő lenyomásával lehet ki-be nyitni. A működést a foldok is megkapták, ráadásul bármelyik sorra kattintva bezáródnak. A képernyőn látható foldok ki vannak emelve, egyben lehet őket kijelölni (nyitó elemre CTRL lenyomása közben kattintással), másolni/beilleszteni másik feliratba, és nem esnek össze idővel. Valódi csoportként működnek most már.
- [Színlevétel](color-picker.md): az egér pozíciójából egy kattintásos új színlevételi módszer
- [Transzformációk](transformations.md): több sor szerkesztése egyszerre photoshophoz hasonló műveletekkel: `Szabad alakítás`, `Torzítás`, `Auto perspektíva`, `Ívelés`, `Hajlítás`, stb
- [Clippelés](vector-clip.md): extra funkcionalitások a clippelésben, autó felismerés AI-al
- [Pipetta mód](clip-eyedropper.md): clip hozzáadása színtartomány szerint
- [Maszkolás](masks.md): maszk létrehozás egyszerűbben, szövegeltávolítás AI-al
- [Alakzatok rajzolása](shapes.md): paint szerű alakzat rajzolás + szabadkezű rajz
- [Színátmenet](gradient.md): vizuális gradient létrehozás, ami egyszerre kezeli a karakterenkénti, az elforgatott és a radiális színátmeneteket, külön arányokban megadható a `\c`, `\3c` és `\4c` + animáció lehetőség (pl. fénycsík végigmenéséhez)
- [Szövegdoboz](text-box.md): képernyőn szerkeszthető szövegdoboz sorkizárt rendezéssel és sormagasság állítással
- [Glitch effekt](glitch.md): különféle glitch effektek létrehozása vizuálisan + animáció lehetőség
- [Motion és Auto motion](motion.md): újragondolt motion kezelés, mely egyszerre kezeli a perspektívát is (akár régi Mochával) és kényelmesebb használatot biztosít Mocha mellett + auto motion
- [Képbeillesztés](image-insert.md): képek beillesztésének lehetősége (PNG-t is támogat áttetszőséggel) + photoshop szerű szerkesztés
- [Vizuális eszközök](visual-tools.md): extra funkcionalitások a videódobozon megjelenő vizuális eszközökben

## Apróbb javítások

> Olyan apróbb javítások listája, melyek már meglévő dolgon javítanak kicsit és nem fért máshova.

- 32-nél több fájlt tartalmazó MKV-k megnyitása
- Videó doboz villogása méretezés közben