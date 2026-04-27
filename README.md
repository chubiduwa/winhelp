# WinHelp

A browser-based viewer for Windows `.hlp` files, the legacy WinHelp format used
across Windows 3.x, 95, 98, NT and early 2000s applications. Parsing is done in
C compiled to WebAssembly; rendering is plain HTML/CSS/JS.

Open a file via the file picker or drag-and-drop. Files are cached locally in
IndexedDB and can be re-opened from the cached-files dropdown. No data ever
leaves the browser.

## Features

**File format**

- Windows 3.x (HC30, HC31) and Windows 95+ (HCW 4.x) help files
- LZ77 and RLE decompression
- v3.0 (`|Phrases`) and v4.0 (`|PhrIndex` + `|PhrImage`) phrase compression
- Internal directory B+ tree traversal
- `|TOPIC` streaming with random-access block map

**Rendering**

- Paragraphs with indents, alignment, borders, tab stops, line spacing
- Font table with size, weight, italic, underline, strikeout, small caps, colors
- Jump links, popup links, macro links (green with WinHelp underline style)
- Tables (cell widths, borders, alignment)
- Non-scrolling and scrolling regions
- Embedded bitmaps (DIB / device-independent) and segmented hypergraphics (SHG)
  with clickable hotspot overlays
- Windows codepages (1250-1258), plus Shift_JIS, EUC-KR, GB2312, Big5 via
  charset detection or LCID fallback
- Right-to-left layout for Arabic and Hebrew files
- Bundled open-source replacement fonts (MS Sans Serif, Tahoma, Courier,
  Fixedsys, Symbol, Wingdings, Marlett)

**Navigation**

- Topic hyperlinks, keyword index search, context / JumpID lookups
- Browse Prev/Next sequences
- Contents, Top, Print, Back toolbar buttons
- URL hash deep-linking (`#file/topic-N` and `#file/index/<query>`)
- File cache to reload without re-upload

**Macros**

- `JumpHash`, `JumpContext`, `JumpID`, `JumpContents`
- `PopupHash`, `PopupContext`, `PopupId`
- `ALink`, `KLink` (with context/hash fallback)
- `CreateButton`, `DisableButton`, `EnableButton`, `DestroyButton`,
  `ChangeButtonBinding`
- `BrowseButtons`, `Back`, `Prev`, `Next`, `Contents`
- `ExecFile` / `EF` (opens `http://`, `https://`, `mailto:` targets)
- `Print`, and common aliases (`JH`, `JC`, `JI`, `PC`, `PI`, `AL`, `KL`, `CB`,
  `DB`, `EB`, `CBB`)
- Chained macros separated by `;`
- Startup macros from `|SYSTEM`

## Not supported

- Vector WMF / EMF metafile rendering (type-8 images with embedded DIBs do
  render; pure GDI command streams do not)
- Full-text search (`|FTINDEX`)
- Multimedia Viewer extensions beyond basic MVB font compatibility
- Cross-file links to external `.hlp` files
- DLL-registered macros
- Annotation (`.ANN`) files
- Contents (`.CNT`) files

## Build

Requires `make`. The Zig toolchain is downloaded automatically on first build.

```
make          # builds dist/hlp.wasm
make fonts    # converts fonts/*.ttf to fonts/woff2/*.woff2 (depends on woff2_compress)
make serve    # build + static server + auto-rebuild watcher (depends on fswatch)
```

## License

See [LICENSE](LICENSE) for attribution of referenced projects
(Wine, helpdeco, Manfred Winterhoff's format documentation) and vendored
dependencies (TLSF, stb, Wine fonts).
