[English](README.md) · **Polski**

# Scene Visualiser

Wizualizator sceny do oświetlenia i ekranów LED, działający na komputerze. Zbuduj rig, spatchuj go, wyślij do niego
Art-Net lub sACN z konsolety i oglądaj w czasie rzeczywistym, jak reagują wiązki i ekrany wideo. Działa w duchu
programu Capture.

![Przykładowy rig z wiązkami i ekranami LED](docs/images/hero.png)

## Możliwości

- **DMX na żywo:** Art-Net 4 i sACN (E1.31) z priorytetami, priorytetem per adres i synchronizacją. Do 64 uniwersów,
  łączonych metodą HTP, LTP lub według priorytetu.
- **Prawdziwe oprawy:** pliki GDTF, także oprawy wielogłowicowe i pikselowe. Wbudowany edytor tworzy proste typy opraw.
- **Wiązki i ekrany na GPU:** wolumetryczne wiązki w hazie, plamy światła na podłodze, gobo i koła kolorów. Ekrany LED,
  ekrany z linek (string walls) i taśmy pikselowe.
- **Wideo:** plansze testowe, obrazy, pliki wideo, przechwytywanie ekranu i okna, NDI, Spout i Syphon. Presety
  Advanced Output z Resolume Arena działają w obie strony.
- **Wymiana danych:**
  - import: MVR, CSV, OBJ, FBX i glTF;
  - eksport: MVR, FBX, OBJ, patch grandMA3 i CSV.
- **Dokumentacja techniczna:** rzut, widok z przodu i z boku, z wymiarami, legendą i tabliczką, eksportowane do PDF
  lub SVG.
- **Narzędzia pokazowe:**
  - programer z grupami i lookami;
  - nagrywanie i odtwarzanie DMX oraz wideo, z podążaniem za timecode;
  - wyjście DMX do sterowania prawdziwym rigiem.
- **Dostęp AI:** wbudowany serwer MCP pozwala asystentowi AI przeglądać i edytować scenę.

## Pobieranie

Instalatory są na stronie [Releases](../../releases):

| System | Plik |
|---|---|
| Windows 10/11 (x64) | `SceneVisualiser-windows.msi` (lub `.zip`, bez instalacji) |
| macOS 13+ (Apple Silicon i Intel) | `SceneVisualiser-macos.dmg` |
| Linux | kompilacja ze źródeł, zob. [docs/building.md](docs/building.md) (po angielsku) |

## Szybki start

1. Zainstaluj i uruchom program. Otworzy się przykładowy rig.
2. Wyślij DMX z konsolety lub programu na ten komputer: Art-Net uniwersum 0:0:0 albo sACN uniwersum 1. Kontrolka
   Art-Net/sACN na pasku tytułu zrobi się zielona.
3. Otwórz **Patch**, aby zobaczyć, na jakim adresie słucha każda oprawa, i przeadresuj je zgodnie ze swoim show.
4. Otwórz **Design**, aby przestawiać obiekty: karta Objects po lewej, Inspector po prawej.
5. Zapisz skrótem Ctrl+S (Cmd+S). Projekt to jeden plik `.svproj`.

## Dokumentacja

| | Polski | English |
|---|---|---|
| Pierwsze kroki | [pierwsze kroki](docs/pl/getting-started.md) | [getting-started](docs/en/getting-started.md) |
| Budowa sceny | [budowa sceny](docs/pl/building-a-rig.md) | [building-a-rig](docs/en/building-a-rig.md) |
| DMX i patch | [DMX i patch](docs/pl/dmx-and-patch.md) | [dmx-and-patch](docs/en/dmx-and-patch.md) |
| Wideo i mapowanie | [wideo i mapowanie](docs/pl/video-and-mapping.md) | [video-and-mapping](docs/en/video-and-mapping.md) |
| Import i eksport | [import i eksport](docs/pl/import-export.md) | [import-export](docs/en/import-export.md) |
| Dostęp AI (MCP) | [dostęp AI](docs/pl/ai-access.md) | [ai-access](docs/en/ai-access.md) |
| Kompilacja ze źródeł | | [building](docs/building.md) |

## Licencja

LGPL-3.0-or-later (zob. [COPYING.LESSER](COPYING.LESSER) i [COPYING](COPYING)). Program używa Qt na licencji
LGPL v3, linkowanego dynamicznie.

Wersja 0.3.0 (Beta).
