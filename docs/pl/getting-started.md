[English](../en/getting-started.md) · **Polski** · [Spis treści](../../README.pl.md#dokumentacja)

# Pierwsze kroki

## Pierwsze uruchomienie

Za pierwszym razem Scene Visualiser otwiera przykładowy rig: podest, dwie kratownice, spoty, washe, beamy, blindery,
belkę pikselową i trzy ekrany LED. Swój projekt otworzysz na trzy sposoby:
- podaj plik `.svproj` przy uruchomieniu;
- upuść go na okno;
- użyj **Project → Open…**.

Jeśli program zamknął się bez zapisu, przy następnym uruchomieniu zaproponuje **Recover**, czyli przywrócenie
ostatniego autozapisu.

## Okno programu

![Widok Design](../images/hero.png)

- **Pasek tytułu:** nazwa projektu (• oznacza niezapisane zmiany), zakładki ekranów i kontrolki Art-Net oraz sACN.
  Przełącznik **3D** wstrzymuje widok 3D, żeby oszczędzać energię.
- **Pasek ikon (po lewej):** Design, Patch, monitor DMX, Control, Plan, Mapping, Project oraz Build (paleta obiektów).
- **Karta Objects (u góry po lewej):** wszystkie obiekty rigu. Kliknięcie zaznacza obiekt, a Shift lub Ctrl dodaje go
  do zaznaczenia.
- **Karta Inspector (u góry po prawej):** pola zaznaczonego obiektu, takie jak pozycja, obrót, typ oprawy, tryb i
  adres.
- **Dok (na dole):**
  - miernik 512 kanałów jednego uniwersum;
  - źródło DMX i jego częstotliwość;
  - **MEDIA**, czyli źródło kompozycji;
  - **REC**, czyli nagrywanie i odtwarzanie.

Obie karty można przeciągać i zwijać. Ich położenie zapisuje się w projekcie.

## Sterowanie kamerą

Na ekranie **Project** wybierz układ sterowania: **Blender** (domyślny) albo **Maya**.

| Czynność | Blender | Maya |
|---|---|---|
| Obrót widoku | przeciąganie środkowym przyciskiem (lub Alt + lewy) | Alt + lewy |
| Przesuwanie | Shift + środkowy | Alt + środkowy |
| Przybliżanie | kółko lub Ctrl + środkowy | Alt + prawy lub kółko |
| Pokaż zaznaczenie | `.` na klawiaturze numerycznej | F |
| Przesuń / obróć / skaluj | G / R / S | W / E / R |

Klawisze wspólne dla obu układów:

| Klawisz | Czynność |
|---|---|
| Num 7 / 1 / 3 / 0 | widok z góry / z przodu / z boku / z kamery |
| Strzałki | obrót o 15° (Shift 5°, Ctrl przesuwa) |
| Home | pokaż całość |
| Przytrzymany prawy przycisk | lot: WASD, Q i E |
| Shift + ` | tryb spaceru |
| `?` lub F1 | lista wszystkich skrótów |

Nie masz klawiatury numerycznej? Na ekranie Project włącz **Emulate numpad**, a cyfry z górnego rzędu przejmą jej
rolę.

## Edycja

Każdą zmianę można cofnąć skrótem Ctrl+Z i ponowić skrótem Ctrl+Shift+Z.

| Skrót | Czynność |
|---|---|
| Ctrl+D | duplikuj zaznaczenie |
| Delete | usuń zaznaczenie |
| Alt + strzałki | przesuń zaznaczenie o 0,25 m (Alt+Page Up/Down w pionie) |
| Escape | wyczyść zaznaczenie |

## Zapisywanie

- **Ctrl+S** zapisuje, a **Ctrl+Shift+S** zapisuje jako. Plik `.svproj` to zip z projektem oraz wszystkimi używanymi
  plikami GDTF, modelami i mediami, więc wystarczy przekazać jeden plik.
- Każdy zapis zostawia obok pliku trzy kopie zapasowe (`.bak1` do `.bak3`).
- Program zapisuje się automatycznie co 2 minuty.
- **RECENT** na ekranie Project pokazuje 10 ostatnich plików.
- Jeśli w projekcie brakuje jakiegoś pliku, przy otwarciu pojawia się okno z jego nazwą.

Dalej: [Budowa sceny](building-a-rig.md)
