# In-game menu translations

The F1 settings menu reads optional translation tables from `locales/<name>.ini`
next to `fefw_rt.dll` (or `fefw.exe`, wherever the binary lives).

## Enabling a language

Set `[gfx] ui_language` in `fefw.ini` to the table name without `.ini`
(`zh` loads `locales/zh.ini`). `auto`, `en`, or an empty value keeps the
built-in English strings.

## Table format

UTF-8 text, one pair per line:

```
English source string = Translated string
```

- The key is the exact English string as it appears in the menu (trimmed).
- Lines starting with `#` or `;` are comments.
- A tab can be used as the separator instead of `=` for keys that themselves
  contain an equals sign.
- A missing key, a partial table, or a broken file is safe: the menu simply
  falls back to the English string.

## Font

Chinese/Japanese/Korean text needs a font that has those glyphs. The runtime
loads the first font it finds from:

1. `[gfx] ui_font` in `fefw.ini` (a path to a `.ttf`/`.ttc` font file),
2. `fonts/ui.ttf` next to the binary,
3. `C:\Windows\Fonts\msyh.ttc` (Microsoft YaHei),
4. `C:\Windows\Fonts\simhei.ttf`, then `Deng.ttf`.

Without one of these the menu renders boxes for CJK characters; English
tables need no font setup.

## Ship checklist for a new language

1. Copy `zh.ini` as a starting point or translate from the English keys.
2. Keep the file UTF-8 **without** BOM.
3. Name it after the language code (`de.ini`, `fr.ini`, ...).
4. Test with `[gfx] ui_language = <code>` and confirm the menu after a game
   restart.
