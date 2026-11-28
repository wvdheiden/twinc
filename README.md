# twinc

(Formerly **ncmac**.) A fast two-panel file manager for the terminal on macOS, Linux and Windows, in the style of the classic DOS commanders: blue panels, function-key bar, command line, and keys your fingers still remember. It is a single native C++17 binary and uses only system libraries. Website: [twinc.tech](https://twinc.tech). (Nederlands: [README.nl.md](README.nl.md))

![twinc: marking files, copying, browsing a zip, the pull-down menu and the viewer](docs/demo.gif)

## Install

**Homebrew**

```sh
brew tap wvdheiden/tap
brew trust wvdheiden/tap        # Homebrew 7+: allow this third-party tap
brew install twinc
```

**macOS installer package:** download the signed and notarized `twinc-<version>.pkg` from [Releases](https://github.com/wvdheiden/twinc/releases). It installs to `/usr/local/bin`.

**Linux packages:** [Releases](https://github.com/wvdheiden/twinc/releases) has a `.deb` for Debian and Ubuntu (`sudo apt install ./twinc_<version>_amd64.deb`) and a plain `.tar.gz` for other distributions, for x86_64 and arm64. They need libarchive (`libarchive13` on Debian/Ubuntu, `libarchive` on Fedora and Arch).

**Windows:** [Releases](https://github.com/wvdheiden/twinc/releases) has `twinc-<version>-windows-x86_64.zip`. Unzip it and put `twinc.exe` in a folder on your `PATH`. It is a single file with nothing else to install, and needs Windows 10 (version 1903 or later) or Windows 11. It runs best in Windows Terminal.

**From source.** You need a C++17 compiler and libarchive; on Linux also its headers (`libarchive-dev` / `libarchive-devel`). On Windows, build in an MSYS2 UCRT64 shell after `pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libarchive mingw-w64-ucrt-x86_64-pkgconf`.

```sh
make && make install PREFIX=~/.local
```

### Shell integration (recommended)

Add one line to `~/.zshrc` or `~/.bashrc`. You then start twinc with `ncm`, and after F10 your shell is in the directory you were in:

```sh
source "$(brew --prefix)/share/twinc/ncm.sh"      # Homebrew
source /usr/local/share/twinc/ncm.sh               # macOS .pkg
source /usr/share/twinc/ncm.sh                     # Linux .deb
```

In PowerShell, add `ncm.ps1` from the Windows zip to your profile (`notepad $PROFILE`):

```powershell
. "C:\Tools\twinc\ncm.ps1"
```

## Terminal setup

* **Linux:** most terminals (GNOME Terminal, Konsole, xterm, kitty) send function keys and Alt as twinc expects. Some desktops claim F1, F10 or Alt-F1…F10 for themselves; switch those shortcuts off in the terminal or desktop settings, or use Ctrl-F (below).
* **Windows Terminal:** function keys, Alt and the mouse work as they are. Alt-Enter (full path on the command line) and F11 are taken by Windows Terminal for full screen. To get Alt-Enter back, remove that shortcut under Settings → Actions.
* **Terminal.app:** Settings → Profiles → Keyboard → enable **Use Option as Meta key**. Alt-F7, Alt+letter quick search and similar keys need this.
* **iTerm2:** Profiles → Keys → Left Option key = **Esc+**.
* **Function keys:** in System Settings → Keyboard, enable **Use F1, F2, etc. keys as standard function keys**, or hold `fn`.
* **No function keys** (for example an iPad keyboard): press **Ctrl-F**, then `1`…`9`, `0` for F1…F10. Put `s`, `a` or `c` before the digit for Shift, Alt or Ctrl. You can also tap the key bar.
* `twinc --keytest` shows the codes your terminal sends for each key.

## Keys

| Key | Action |
|---|---|
| F1 F2 F3 F4 | help, user menu, view, edit |
| F5 F6 F7 F8 | copy, rename/move, mkdir, delete (to Trash) |
| F9 F10 | pull-down menu, quit |
| Shift-F4 / Shift-F6 / Shift-F8 | edit new file, rename in place, delete permanently |
| Alt-F1 / Alt-F2 | choose a volume or favourite for the left/right panel |
| Alt-F7 / Alt-F8 / Alt-F9 / Alt-F10 | find file, history, 25/43/50 lines, directory tree |
| Ctrl-F1 / Ctrl-F2 | left/right panel on/off |
| Ctrl-F3 … F7 | sort by name / extension / time / size / unsorted |
| Tab | other panel |
| Insert, Ctrl-T | mark file |
| `+` `-` `*` | select group, unselect group, invert selection |
| Alt+letter, Ctrl-S | quick search |
| Ctrl-O | hide panels (user screen with its own prompt) |
| Ctrl-P / Ctrl-U / Ctrl-L / Ctrl-R | other panel on/off, swap, info, re-read |
| Ctrl-PgUp / Ctrl-PgDn / Ctrl-\\ | parent directory, enter directory/archive, root |
| Ctrl-E / Ctrl-X | previous / next command |
| Ctrl-J / Alt-Enter | put file name / full path on the command line |
| Shift-F9 | save setup |
| Alt-. | show/hide hidden files (dotfiles and Finder-hidden; shown dimmed), also F9 → Options |

**Enter** opens what is under the cursor:
* a directory or an archive is shown in the panel;
* a script is run;
* a program (`.exe`, `.bat`, `.cmd`, `.ps1` on Windows) is run;
* anything else opens in its default app (`open` on macOS, `xdg-open` on Linux, the associated app on Windows). Without a desktop, for example over SSH, it opens in the built-in viewer.

**Archives** open like directories, read-only. Supported formats: zip, tar.gz/bz2/xz/zst, 7z, rar, iso, cpio, xar, cab, jar, deb, rpm and more (via the system libarchive). F3 views a file in the archive and F5 extracts files to the other panel.

**F8** moves to the Trash, the same way Finder does. On Linux it uses the desktop Trash (`~/.local/share/Trash`) and on Windows the Recycle Bin, so you can restore from your file manager. You can switch this off under F9 → Options.

**Mouse:** click to select, double-click to open, right-click to mark, scroll with the wheel. Clicking the key bar presses that key.

## User menu (F2)

The menu lives in `~/.config/twinc/menu` (on Windows `%APPDATA%\twinc\menu`); a `.ncmenu` file in a directory takes precedence there. Commands run in your shell: `$SHELL` on macOS and Linux, `cmd.exe` on Windows.

```
g: Git status
    git status
z: Zip selection
    zip -r archive.zip %s
```

Substitutions: `%f` current file, `%s` selection, `%d` current directory, `%o` the other panel's directory.

## License

MIT, see [LICENSE](LICENSE).
