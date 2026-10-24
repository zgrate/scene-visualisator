[English](../en/video-and-mapping.md) · **Polski** · [Spis treści](../../README.pl.md#dokumentacja)

# Wideo i mapowanie

Ekrany LED wyświetlają **kompozycję**: jeden obraz, z którego każdy ekran pobiera swój wycinek (slice).

## Źródło kompozycji

Kliknij **MEDIA** w doku, aby wybrać źródło:

| Źródło | Uwagi |
|---|---|
| Plansza testowa / siatka | wbudowane; siatka wypisuje współrzędne, co pomaga sprawdzić mapowanie |
| Obraz | umieszczany w trybie fit, fill albo stretch |
| Wideo | plik wideo w pętli. Pliki do 200 MB są kopiowane do projektu. Większe są tylko linkowane. |
| Ekran / okno | przechwytywanie monitora albo okna jednej aplikacji |
| NDI | dowolny nadajnik NDI w sieci. Wymaga zainstalowanego darmowego [NDI runtime](https://ndi.video/tools/). |
| Spout (Windows) / Syphon (macOS) | tekstura GPU udostępniana przez inny program |

Karta Media pokazuje rozdzielczość, liczbę klatek i opóźnienie.

## Ekran Mapping

![Ekran Mapping](../images/mapping.png)

Kompozycja z naniesionymi wycinkami wszystkich ekranów. Przeciągnij wycinek, aby go przesunąć, albo wpisz dokładne
wartości w pikselach w Inspectorze.

## Resolume Arena

- **Import:** użyj **Project → IMPORT → Resolume screens…** (albo upuść plik `.xml`), aby otworzyć preset Arena
  *Advanced Output*. Tworzy on wyjścia, ekrany LED i taśmy pikselowe oraz ustawia rozmiar kompozycji.
  - Z opcją **Place from composition** ekrany są rozstawiane według wycinków, przy podanym rastrze LED (`pitch mm`).
  - Ekran o nazwie zgodnej z istniejącym aktualizuje ten istniejący.
- **Eksport:** **Resolume output…** zapisuje preset z powrotem. Ustawienia, których program nie używa, zostają
  dokładnie takie jak były, więc Arena odczyta je bez zmian.
- **Send to Resolume:** zapisuje preset prosto do `Documents/Resolume Arena/Presets/Advanced Output/`.
- **Osobne strumienie wyjść:** Arena może wysyłać jeden strumień NDI na każdy ekran wyjściowy. **Connect NDI** w karcie
  Media dopasowuje je po nazwie, więc każdy ekran pokazuje dokładnie to, co Arena wysyła na jego wyjście.

## Taśmy pikselowe i ekrany z linek

- **Taśma pikselowa (pixel strip):** taśma LED sterowana z DMX (piksel po pikselu, w razie potrzeby przez kilka
  uniwersów) albo wycinkiem kompozycji.
- **Ekran z linek (string wall):** ekran LED złożony z łamanych. Ustawienie **Facing** decyduje, czy świeci w jedną
  stronę, czy na wszystkie.

Dalej: [Import i eksport](import-export.md)
