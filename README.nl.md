# ncmac

Een two-panel filemanager voor de Mac-terminal in de stijl van de klassieke DOS-commanders. Eén native binary in C++17, geen dependencies. (English: [README.md](README.md))

## Installeren

```sh
brew install wvdheiden/tap/ncmac          # via Homebrew
```

Of download de ondertekende `ncmac-<versie>.pkg` van de GitHub-releases, of bouw zelf:

```sh
make && make install PREFIX=~/.local
```

Zet daarna dit in `~/.zshrc`. Dan start je met `ncm`, en na F10 staat je shell in de map waar je in ncmac was:

```sh
source "$(brew --prefix)/share/ncmac/ncm.zsh"   # Homebrew
source /usr/local/share/ncmac/ncm.zsh            # .pkg
```

`nc` zelf is op macOS al netcat; wil je toch `nc` typen, zet dan `alias nc=ncm` in je `.zshrc`.

## Terminal-instellingen (belangrijk)

* **Terminal.app** → Instellingen → Profielen → Toetsenbord: vink **"Gebruik Option als Meta-toets"** aan (voor Alt-toetsen zoals Alt-F7 en Alt+letter quick search).
* **iTerm2** → Profiles → Keys: Left Option = **Esc+**.
* Systeeminstellingen → Toetsenbord: **"Gebruik F1, F2 enz. als standaard functietoetsen"**, anders moet je `fn` erbij houden. Mission Control pakt sommige Ctrl-F-toetsen af; alles zit ook in het F9-menu.
* Geen F-toetsen (iPad)? **Ctrl-F, dan 1…9, 0** = F1…F10. Zet `s`, `a` of `c` vóór het cijfer voor Shift, Alt of Ctrl (Ctrl-F `a` `7` = Alt-F7). Je kunt ook op de toetsenbalk tikken, of snel `Esc 1` … `Esc 0` typen.
* `ncmac -s 80x25` zet het terminalvenster op 80×25 (en zet het bij afsluiten terug). Alt-F9 schakelt 25/43/50 regels.

## Toetsen

| Toets | Functie |
|---|---|
| F1 / F2 / F3 / F4 | help / user menu / view / edit |
| F5 / F6 / F7 / F8 | copy / rename-move / mkdir / delete (naar de Prullenmand) |
| Shift-F8 | definitief verwijderen (in de Prullenmand zelf is F8 ook definitief) |
| F9 / F10 | pull-down menu / quit |
| Shift-F4, Shift-F6 | nieuw bestand editen, hernoemen ter plekke |
| Alt-F1 / Alt-F2 | "drive" kiezen (volumes in /Volumes, home, favorieten) |
| Alt-F7 / Alt-F8 / Alt-F9 | find file / history / 25-43-50 regels |
| Alt-F10 | directory tree: →/`+` uitklappen, ←/`-` inklappen of naar ouder, `*` alles eronder uitklappen, typen = zoeken, Enter = ga erheen |
| Ctrl-F1 / Ctrl-F2 | linker/rechter paneel aan/uit |
| Ctrl-F3 … F7 | sorteren op naam / extensie / tijd / grootte / ongesorteerd |
| Tab | ander paneel |
| Insert, Ctrl-T | bestand markeren |
| `+` `-` `*` | groep selecteren / deselecteren / inverteren (met lege commandoregel) |
| Alt+letter, Ctrl-S | quick search |
| Ctrl-O | panelen weg: user screen met uitvoer en een eigen prompt |
| Ctrl-P / Ctrl-U / Ctrl-L / Ctrl-R | ander paneel aan/uit / panelen wisselen / info / herlezen |
| Ctrl-PgUp / Ctrl-PgDn / Ctrl-\\ | map omhoog / map in (ook .app) / root |
| Ctrl-E / Ctrl-X | vorig / volgend commando |
| Ctrl-J (Ctrl-Enter) / Alt-Enter | bestandsnaam / volledig pad op de commandoregel |
| Esc | commandoregel wissen |
| Shift-F9 | setup opslaan |

Enter op een bestand: een map gaat open; een archief gaat open als map; een script wordt uitgevoerd; al het andere opent met de bijbehorende Mac-app (`open`).

### Archieven

Enter op een zip, tar(.gz/.bz2/.xz/.zst), tgz, 7z, rar, iso, cpio, xar, cab, jar, deb, rpm … opent het archief als map (via de libarchive van macOS). Ctrl-PgDn probeert élk bestand als archief te openen. Binnen een archief:

* bladeren, quick search, markeren werken gewoon; `..` in de bovenste map brengt je terug
* F3 bekijkt een bestand, Enter opent het met de Mac-app (via een tijdelijke kopie)
* F5 kopieert (uitpakken) naar het andere paneel, ook hele mappen
* archieven zijn alleen-lezen: F4/F6/F7/F8 geven een melding
* entries met `../` of absolute paden worden genegeerd (geen "zip slip") `cd`, `cd..`, `cd -` en `cd ~` worden intern afgehandeld.

Muis: klik selecteert, dubbelklik opent, rechtsklik markeert, het wiel scrollt en een klik op de toetsenbalk drukt die toets in.

## User menu (F2)

`~/.config/ncmac/menu` (een `.ncmenu` in een map gaat voor). Formaat:

```
g: Git status
    git status
z: Zip selectie
    zip -r archive.zip %s
```

`%f` huidig bestand, `%s` selectie, `%d` huidige map, `%o` map van het andere paneel, `!.!` = `%f`.

## Bestanden

* `~/.config/ncmac/ncmac.conf`: setup (Shift-F9)
* `ncmac --keytest`: laat zien welke code je terminal voor een toets stuurt en hoe ncmac die leest
* `~/.config/ncmac/history`: commandogeschiedenis

F8 gebruikt dezelfde Prullenmand als Finder, ook die op USB- en externe schijven. Lukt dat niet (bijvoorbeeld op een netwerkschijf), dan vraagt ncmac of het definitief mag. Uitzetten kan via F9 → Options → "Delete to Trash" (en Shift-F9 om dat te bewaren).
