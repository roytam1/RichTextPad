# RichTextPad

Minimal Win32 **C89** rich text editor in a single C file (`richedit.c`,
~4800 lines, no external dependencies beyond system libraries).

## Features

- Native Rich Edit control: **Msftedit 4.1+** (`RICHEDIT50W`) with
  automatic fallback to `RICHED20.DLL` (RichEdit 2.0/3.0)
- Open / Save / Save As for **RTF** (`.rtf`)
- **Markdown** files (`.md`, `.markdown`, `.mkd`, `.mdown`, `.txt`)
  through a built-in md↔RTF layer:
  headings, **bold**, *italic*, inline `` `code` `` and code blocks,
  bullet and numbered lists, blockquotes, rules, links, **tables**,
  UTF-8 (`\uN` RTF encoding)
- `Edit → Show Markdown Source` checkable toggle: WYSIWYG rich view
  vs. plain Markdown source in the same control
- **Clickable links**: `EN_LINK` opens the default browser
  (`ShellExecute`), `CFE_LINK` effects applied after load so link
  display text is clickable on old controls too
- Command-line file open (`richedit.exe notes.md`), filename shown
  in the title bar (`RichTextPad - notes.md`)
- Basic Edit menu: Undo, Cut, Copy, Paste, Select All

## Build

MSVC (a `mk.bat` wrapper is included, VC6-era compatible):

```bat
cl /Ox richedit.c user32.lib comdlg32.lib shell32.lib
```

MinGW:

```sh
gcc -std=c89 -mwindows richedit.c -o richedit.exe \
    -luser32 -lgdi32 -lcomdlg32 -lshell32
```

The code is strict C89 (declarations first, no `//` comments,
no `snprintf`) and compiles warning-clean apart from pre-existing
64-bit `DWORD_PTR`/callback cast notes.

## Usage

- `File → Open…` offers Markdown, RTF and All Files filters;
  `Save As` defaults to `document.md` (or keeps the current extension).
- Saving always follows the file extension: `.md*`/`.txt`
  goes through the RTF→Markdown converter, `.rtf` stays RTF —
  regardless of which view is active.
- In source view the editor uses a fixed-width font with default
  style/size; toggling back re-renders the Markdown as rich text.
- Clicking a link opens it in the default browser. On old RichEdit
  (2.0/3.x) `HYPERLINK` fields are expanded by the control itself to
  `display <url>` plain text; the URL part (and, after load, the
  display part) stays clickable.

## Markdown round-trip notes

The converters cover a deliberate subset and normalize on load:

- Fenced ` ``` ` blocks become indented code blocks
  (stable afterwards); inline code stays inline.
- Escaping is minimal and idempotent: intra-word `_`
  (e.g. `test_avif2pnm`) is left raw, structural characters
  (`*`, `` ` ``, `[`, `]`, `|`) are escaped only where needed,
  so repeated rich↔source toggles don't accumulate slashes.
- Old controls (2.0/3.x) renumber fonts and expand fields;
  `rtf_to_md` maps `\fN` to monospace by font-table **name**
  (not index) and skips `{\*\generator}`, `{\*\themedata}` and
  other `{\*…}` destinations, so code blocks and links survive.
- Not supported: nested lists, images (kept as links),
  reference-style links, footnotes, strikethrough, task lists.

## Layout

```text
richedit.c   single-file application
mk.bat       MSVC build wrapper
```

`stb_avif_README.md` in this directory is only a manual test
document (links, code fences, lists, UTF-8 dashes).
