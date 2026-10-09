# TraceLab-iconen

Drie zwarte wiggle-traces met gevulde positieve lussen op een oranje tegel
met een rond verloop (#ffc08a → #ff9650 → #d95a1e), in de signaalkleur van
de app. De bestandsnamen beginnen nog met `segyread`, zodat app.rc en
app_icon.qrc niet hoeven te veranderen.

## Inhoud

| Map | Wat |
|---|---|
| `windows/segyread.ico` | Windows-icoon met 16, 20, 24, 32, 40, 48, 64, 96, 128 en 256 px |
| `linux/hicolor/…` | Linux-icoonthema (16–512 px + scalable SVG) |
| `linux/segyread.desktop` | Voorbeeld-starter voor het applicatiemenu |
| `png/` | Losse PNG's, 16 t/m 1024 px, transparante hoeken |
| `png/segyread-mark-*.png` | Alleen de traces in oranje, transparant (splash/About-scherm) |
| `svg/segyread.svg` | Master, schaalbaar (vanaf ~40 px) |
| `svg/segyread-small.svg` | Voor 16–32 px (dikkere traces) |
| `svg/segyread-mark.svg` | Alleen de traces, schaalbaar |

16–32 px zijn bewust vereenvoudigd: de volledige lijn wordt op die maat een
waas. Gebruik voor kleine weergaven dus de PNG's of de `.ico`, niet een
verkleinde master.

## Windows

- **Exe-icoon:** koppel `windows/segyread.ico` aan je build, bijvoorbeeld
  - PyInstaller: `pyinstaller --icon windows/segyread.ico …`
  - C/C++ (resource-bestand): `IDI_ICON1 ICON "segyread.ico"`
  - .NET: `<ApplicationIcon>segyread.ico</ApplicationIcon>` in het `.csproj`
- **Venster-icoon (Qt):** `app.setWindowIcon(QIcon("segyread.ico"))`

## Linux

Voor één gebruiker:

```sh
cp -r linux/hicolor ~/.local/share/icons/
cp linux/segyread.desktop ~/.local/share/applications/
gtk-update-icon-cache ~/.local/share/icons/hicolor 2>/dev/null || true
```

Systeembreed: dezelfde bestanden naar `/usr/share/icons/hicolor/` en
`/usr/share/applications/`. Pas `Exec=` in het `.desktop`-bestand aan naar
het pad van je programma. Venster-icoon in Qt:
`QIcon.fromTheme("segyread")` of `QIcon("png/segyread-256.png")`.
