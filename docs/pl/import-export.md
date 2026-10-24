[English](../en/import-export.md) · **Polski** · [Spis treści](../../README.pl.md#dokumentacja)

# Import i eksport

Import i eksport są w sekcjach **IMPORT** i **EXPORT** na ekranie Project. Plik można też upuścić na okno: `.svproj`,
`.gdtf`, `.mvr`, `.obj`, `.fbx` lub `.xml` (Resolume).

## Formaty

| Format | Import | Eksport | Uwagi |
|---|---|---|---|
| MVR 1.6 | ✓ | ✓ | Oprawy, ekrany LED, kratownice, grupy i geometria scenografii. Import dołącza do otwartego projektu. |
| CSV | ✓ | ✓ (patch) | Okno dialogowe przypisuje kolumny. Oprawy o pasujących numerach są aktualizowane, pozostałe dodawane. |
| OBJ (+ MTL) | ✓ | ✓ | Statyczne elementy scenografii z kolorami materiałów. Przy imporcie wybierz jednostkę i Z-up, opcjonalnie rozdziel grupy. Eksport całości albo zaznaczenia. |
| FBX | ✓ | ✓ | Siatki zachowują hierarchię. Eksport dodaje ekrany LED i kamery, dla Blendera, Unreala lub C4D. |
| glTF / GLB | ✓ (w MVR) | ✓ (w MVR) | |
| Patch grandMA3 | | ✓ | XML opraw dla MA3 |
| Preset Resolume | ✓ | ✓ | Zob. [Wideo i mapowanie](video-and-mapping.md) |

Jednostki i oś pionowa są przeliczane przy imporcie. Program pracuje w metrach, z osią Y w górę.

## Rysunki

![Rzut sceny](../images/plan.png)

Ekran **Plan** rysuje rzut, widok z przodu i z boku na arkuszu A3 lub A4, z:
- symbolami opraw według typu i opisami;
- automatycznymi wymiarami (rozmiar sceny, rozstaw opraw, wysokości kratownic);
- legendą z liczbą sztuk, opcjonalną tabelą opraw i tabliczką rysunkową.

Edycja arkusza:
- przeciąganie z Shift dodaje wymiar;
- **N** wstawia notatkę, a dwuklik zmienia jej tekst;
- przeciąganie opraw przesuwa je w rigu.

**Export PDF** i **Export SVG** zapisują arkusz.

## Nagrywanie pokazu

Kliknij **REC** w doku, aby nagrywać. Program zapisuje:
- każde odbierane uniwersum do pliku `.svdmx`;
- wideo kompozycji i każdy strumień wyjścia na żywo do plików `.mp4`, jeśli działa źródło na żywo.

**Playback** odtwarza nagranie. Może je zapętlać albo podążać za timecode Art-Net z przesunięciem. Można wybrać, które
oprawy, grupy lub wyjścia grają z nagrania, a które zostają na żywo, i płynnie przechodzić między nimi. Nagrania leżą
obok projektu, nie w nim.

Dalej: [Dostęp AI](ai-access.md)
