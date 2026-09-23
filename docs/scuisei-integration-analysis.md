# scuisei-rs beépítési elemzés

Dátum: 2026-09-22.

Vizsgált upstream: `eldonishere/scuisei-rs`, `feature/perf` ág,
`e4de7913c93e2068b88db3045ccd0144b547e5b8` commit, Cargo-verzió: 0.1.8.
Az első ütem megvalósult; az alábbiak az upstream elemzését és a beépítés
döntéseit rögzítik. Valós videókon a felismerés pontossága továbbra is mérendő.

## Javaslat

A közvetlen beépítés első lépése a
**Videó → Kulcskockák betöltése a videóból** parancs.
Ez végigelemzi a megnyitott videót, és a felismert jelenetváltásokat betölti
az Aegisub közös kulcskockalistájába. A feliratsorok időzítése ekkor nem változik.

A második lépés az időzítő javító, a felhasználó által később átadott új
Lua-script alapján. A jelenlegi `nyaa.fix-timing.lua` nem módosítandó,
és nem tekintendő az új időzítő végleges specifikációjának.

## Mit ad a scuisei?

- A képtartalomból jelenetváltásokat keres; ezek különböznek a videó
  kódolásából származó I-/kulcskockáktól.
- FFmpeg segítségével dekódol, majd alaphelyzetben legfeljebb 160 × 96 méretű,
  képarányt megtartó fényességképen elemez. Mozgásbecslést, hisztogramokat,
  adaptív küszöböket és utófeldolgozást használ.
- A finomított eredmény az `analyze_keyframes()` vagy az
  `AnalysisResult.keyframes`. Az `agi` és `frames` kimenet ezt használja.
- A `pass_decisions` / `--format xvid` eltérő, utófeldolgozás előtti döntéseket
  tartalmazhat. Az upstream saját tesztje is mutat eltérést a két eredmény között.
  A beépítéshez ezért a finomított listát javaslom.
- A képkockaszámozás nullától indul. Nem kell hozzáadni egyet.
- A kibocsátott AGI formátumot a jelenlegi Aegisub olvasója fel tudja dolgozni:
  soronként a kezdő egész számot olvassa ki, így az `I -1` utótag nem akadály.
- Vannak automatikus tesztek és két anime-videós referencia. Ez jó kiindulás,
  de önmagában nem igazol általános vagy minden képkockára pontos felismerést;
  a Bleach-teszt például két képkocka eltérést is elfogad.

Források: [API és kimenetek](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/src/lib.rs),
[elemzési folyamat](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/src/analysis.rs),
[API-tesztek](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/tests/library_api_test.rs),
[pontossági teszt](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/tests/bleach_fixture_test.rs).

## A közvetlen beépítés módja

A Rust detektormag rögzített upstream commitból fordul statikus könyvtárrá,
keskeny C ABI illesztőréteggel. Az eredeti `detector`, `simd_metrics`,
`postprocess`, `validation` és `error` modul változtatás nélkül kerül be.
A Meson wrap URL-je és SHA-256 ellenőrzése rögzíti a forrást; a külön adapter
és Cargo-projekt a `subprojects/packagefiles/scuisei` könyvtárban található.

A jelenlegi upstream csak Rust `rlib` kimenetet ad, nincs kész C/C++ API-ja.
A saját adapter állapottartó create/push/finish/destroy API-t ad, ellenőrzi a
bemeneti képpuffert, és hibakódokkal tér vissza. A C++ RAII osztály birtokolja
az állapotot és lemásolja az eredményt. A külön Cargo-profil `panic = "unwind"`,
az ABI-határokon `catch_unwind` védi a hívót.

Az upstream `progress` kapcsoló terminálos kijelzést vezérel. Nincs nyilvános
GUI-callback vagy megszakítási token. A beépített változatban a C++ vezérli a
képkockánkénti feldolgozást, frissíti a folyamatjelzőt, és minden dekódolás
előtt és után ellenőrzi a megszakítást. Az éppen folyó képkocka-dekódolás és
az utófeldolgozás befejezését a Mégse megvárja; a főablak addig tiltva marad.

Az adapter nem használja az upstream FFmpeg-dekóderét vagy CLI-jét.
Az Aegisub `AsyncVideoProvider` szolgáltatja a nyers, felirat nélküli BGRX
képeket egyetlen soros feldolgozási feladatban, a kijelző gyorsítótárát
megkerülve. Az FFmpegSource már az FFMS kimenetét legfeljebb 160 × 96 méretre
állítja pontmintavételezéssel, elkerülve a teljes méretű RGB-konverziót és
képmásolatokat. Más providereknél a hagyományos képkockakérés az alapértelmezett.
Így ugyanaz a videosáv, VFR-számozás és szkriptelt bemenet kerül
elemzésre, amelyet a felhasználó megnyitott; nincs második FFmpeg ABI.
A Rust adapter legfeljebb 160 × 96 méretre mintavételez, majd teljes
tartományú BT.601 fényességet számít a BGR komponensekből.
Az FFMS pontmintavételezési helyei kismértékben eltérhetnek a Rust adapterétől.
A BGR-ből számított fényesség eltérhet az upstream közvetlen YUV-mintáitól is,
ezért az önálló
scuisei CLI-vel képkockára azonos eredmény nem garantált. A detektor és az
utófeldolgozás alapértelmezett küszöbei az upstream értékei.

Alternatíva az alkalmazással szállított külön scuisei-folyamat. Ez egyszerűbb
hibaszigetelést és leállítást ad, viszont nem az alkalmazásba linkelt megoldás.
A teljes algoritmus C++-ra átírását első lépésként nem javaslom: jelentős
karbantartási és eredményazonossági többletmunkát okozna.

Források: [Cargo-beállítások](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/Cargo.toml),
[dekóder](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/src/decoder.rs),
[platformos CI](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/.github/workflows/ci.yml).

## A menüpont működése

1. A `src/command/keyframe.cpp` új parancsa a videómenüben jelenik meg.
   Megnyitott, támogatott videó nélkül nem indítható.
2. Az Aegisub meglévő `DialogProgress` ablaka **modálisan** jelenik meg,
   és az elemzés végéig letiltja az alkalmazás többi ablakát. A videó- és
   hanglejátszás leáll. A folyamatjelző és a Mégse gomb közben működik.
   A feldolgozó szál nem módosítja közvetlenül a GUI állapotát.
3. Az eredményt sikeres befejezéskor ellenőrizzük: rendezett, egyedi,
   nemnegatív, a videó képkockatartományába eső egész számok.
4. Csak a teljes, érvényes eredmény cserélheti le az aktuális listát.
   Hiba és megszakítás esetén megmarad a korábbi lista.
5. A lista a `Project` közös kulcskockaállapotába kerül, és ugyanaz az értesítés
   frissíti a videós és audiós nézeteket, mint fájlból betöltéskor.
   Az Automation `aegisub.keyframes()` is ezt az állapotot olvassa.
6. A generált lista menthető és bezárható. A `?user/scuisei-keyframes`
   könyvtárban minden sikeres elemzés egyedi, tartós AGI fájlt kap;
   a mentett ASS erre hivatkozik, így újranyitáskor is elérhető.
7. Nincs automatikus eredmény-újrafelhasználás: minden indítás új elemzés.
   A tartós fájlok nem törlődnek automatikusan, mert mentett projektek
   hivatkozhatnak rájuk. Más gépre költöztetéskor ezeket is másolni kell,
   vagy a szokásos Kulcskockák mentése paranccsal a projekt mellé menteni.

## Platformok és korlátok

- **Bemenet:** minden képkockát szolgáltató Aegisub-videóprovider használható,
  a dummy és szkriptelt videók is. Az elemzéshez nem kerül fájlnév a Rust API-ba.
- **Memória:** csak az előző lekicsinyített képet és a képkockánkénti
  statisztikákat tároljuk; a teljes dekódolt videót nem.
- **Visszaállítás:** az FFMS kimeneti felbontása siker, megszakítás és kivétel
  után is visszaáll a normál értékre. A következő kijelzési képkockakérés
  szintén biztosítja a normál formátumot. A korábbi kijelzési cache megmarad.
- **Fordítás:** Rust/Cargo 1.93 vagy újabb szükséges. A függőségeket Cargo.lock
  rögzíti, a build `--locked` módban fut. A Meson a platform natív Rust
  célarchitektúrájához épít; Windows MSVC, Linux és Intel/Apple Silicon macOS
  ágak állnak rendelkezésre. A CI telepíti a toolchaint, és Linuxon PCH nélkül
  is fordít. Keresztfordítás külön toolchain-beállítást igényel.
- **Terjesztés:** a detektor és az adapter MIT-licencű. A beépített Rust
  függőségek és a standard könyvtár licencszövegei az EXE-be kerülnek,
  az Aegisub Névjegy ablakában olvashatók. Külön scuisei.exe vagy FFmpeg DLL
  nem szükséges ehhez a funkcióhoz.

Forrás: [upstream FFmpeg-tájékoztató](https://github.com/eldonishere/scuisei-rs/blob/e4de7913c93e2068b88db3045ccd0144b547e5b8/NOTICE-FFMPEG.md).

## Elfogadási ellenőrzések az első megvalósításhoz

Teljes Windows-build és telepített EXE hash-ellenőrzés; Linux-build PCH nélkül;
Intel és Apple Silicon macOS build. Funkcionális próbák: ismert jelenetváltások,
első és utolsó képkocka, VFR, több videosáv, 10 bites videó, Unicode fájlnév,
sérült bemenet, megszakítás, modális tiltás, projekt újranyitása,
lista mentése és visszaállítása. Meglévő lista nem veszhet el sikertelen elemzéskor.

## Beépített intelligens időzítő

Az Időzítés menü harmadik eleme a **Kijelölt sorok intelligens javítása...**
(`time/smart_fix`). A felhasználótól kapott `Dynamo.OkosIdozites.lua` 2.1.4
változata változatlan algoritmussal a program erőforrásai közé kerül
(`src/libresrc/smart_timing.lua`). Nem igényel telepített autoload scriptet;
a korábbi `nyaa.fix-timing.lua` fájlt nem módosítja.

A parancs megnyitott videót, érvényes időbélyegeket és legalább egy kijelölt,
nem komment sort igényel. Meglévő betöltött kulcskockalistát használ. Ha nincs
ilyen lista, vagy üres, először a modális jelenetfelismerést futtatja; a videó
saját kódolási I-kockái önmagukban nem számítanak betöltött jelenetlistának.
A jelenetfelismerés megszakítása vagy hibája esetén az időzítő nem indul el.

Ezután megjelennek a script eredeti beállításai. Az alapértékek csak az alsó
beszédsorokat javítják, kihagyják a komplex/animált sorokat, védik a nem
kijelölt szomszédokat, és figyelembe veszik a minimum időtartamot és a CPS-t.
Az időzítő az Aegisub meglévő Automation tranzakcióját és visszavonását
használja. A `dynamo.smart_timing` extradata-jelölő az ASS-be is elmenthető;
az eredeti időkből való újraszámolás megakadályozza az ismételt eltolódást.

A beágyazott Lua-állapotot a natív parancs birtokolja, így a beállítások az
alkalmazás futása alatt megmaradnak. Makrói privát tulajdonban vannak:
nem kerülnek be az Automation menübe vagy a legutóbbi makrók listájába,
és az autoload scriptek újratöltése nem törli őket.

Kilenc célzott teszt ellenőrzi a rés- és átfedéskezelést, a KF-igazítás
korlátait, a kijelölési széleket, a kihagyott sorokat, a minimum időtartamot,
a megszakítást, az ismételt futtatást, a megváltozott kulcskockalistát és
az Aegisub valódi VFR-időkonverzióit.

A Windows felületi próba során a beágyazott script beállítási és összegző
ablaka megfelelő méretben, ékezethelyesen jelent meg. Egy 25 fps-es dummy
videóval, betöltött lista nélkül indítva létrejött a tartós kulcskockafájl;
a két kijelölt beszédsor 300 ms-os rése a várt 2,24 s-os közös határra
került, a kijelölt komment változatlan maradt. A mentett ASS tartalmazta a
kulcskockahivatkozást és az extradata-jelölőket. Egy visszavonás visszaállította
mindkét eredeti időzítést. A második próba meglévő 0/25/50-es listáját a
parancs megtartotta, és az 1,03–2,07 s-os sort a várt 0,98–1,98 s-ra igazította.
A beállítási ablak Mégse/Escape lezárása nem módosította a feliratot.

Az időzítőt tartalmazó teljes Windows-build és a Linux-build (`b_pch=false`)
is sikeres. A 23 célzott teszt mindkét platformon lefutott: 9 időzítő-,
4 Lua-betöltési, 4 detektor- és 6 kulcskockafájl-teszt. A Windows EXE
és másolata SHA-256 szerint azonos; az új magyar feliratokat mindkét
MO-fájlban ellenőriztük. A Linux-környezet korábbi FFmpeg/ICU
linkerfigyelmeztetései változatlanul jelen vannak.

## Elvégzett ellenőrzések és teljesítménymérés

- Windows: teljes build, az EXE és a telepített másolat SHA-256 azonossága;
  a hat új magyar üzenet mindkét MO-fájlban helyes UTF-8 szöveggel szerepel.
- Linux: a gyorsított változat teljes buildje is sikerült, `b_pch=false`
  beállítással. A helyi környezet meglévő FFmpeg/ICU verzióütközési
  linkerfigyelmeztetései megmaradtak. macOS-build helyben nem futott;
  mindkét architektúra meglévő CI-feladata megmaradt és Rust-telepítést kapott.
- Detektor: 26 Rust-teszt Windows és Linux alatt; a C ABI-ra 4 C++ teszt
  (statikus videó, pontos vágás, hibás puffer, fordított sorok).
- Kulcskockafájlok: a kapcsolódó tesztek Windows és Linux alatt is futottak;
  a Windows-sorvéges AGI-fejléc Linuxon feltárt olvasási hibáját javítottuk.
- Windows felület: 500 000 képkockás dummy tesztben modális tiltás, frissülő
  folyamatjelző, megszakítás az eredeti lista megtartásával, teljes lefutás,
  generált lista betöltése és a tartós fájl hivatkozásának ASS-be mentése.
  Ez a próba a provider teljesítményoptimalizálása előtt történt.
- Izolált FFMS + detektor mérés WSL/Linux alatt, 1920 × 1080, H.264,
  720 képkocka, 4 dekóderszál, FFMS kimenet + képmásolat + detektor ideje:

  | Bemenet | Teljes RGB képkocka + cache-másolat | Kicsinyített RGB, cache nélkül |
  | --- | ---: | ---: |
  | Mozgó `testsrc2` | 6,86–6,87 s | 0,40–0,41 s |
  | Fekete–fehér–fekete szakaszok | 5,79–5,86 s | 0,19–0,20 s |

  A mozgó próba régi idejéből a detektor mindössze 0,09–0,10 s volt;
  a többlet túlnyomó része a teljes RGB-képkocka előállításából származott.
  A statikus szakaszok mindkét úton pontosan a 0., 240. és 480. képkockát
  adták; a mozgó teszt az első képkockát. A mérés után a normál 1920 × 1080
  FFMS-kimenet visszaállítását és újabb képkockakérést is ellenőriztük.
  Ezek szintetikus, ismételt mérések, nem általános sebességígéretek
  vagy közvetlen SCXvid-összehasonlítások. Valós tömörítésnél, más providernél
  és hardveren eltérhet a nyereség.
