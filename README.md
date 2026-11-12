# PSD Simple Editor

Views PSD/PSB files (each layer on its own, or the file's stored flattened preview), renames
layers and layer groups, and moves them into, out of and between groups. Saving rewrites only the
renamed layers' name fields and the enclosing length fields; moved layers are copied unchanged to
their new positions, and everything else is copied byte for byte.

## Build

```
build.cmd [release|debug]      -> build\x64-<config>\PSDSimpleEditor.exe
```

Or open the folder in Visual Studio 2026 (CMake presets `x64-debug` / `x64-release`).

You need VS 2026 C++ tools, CMake ≥ 3.25, the Windows SDK (its `dxc` compiles the shaders), and
vcpkg at `C:\pig\vcpkg` with `qtbase` (Widgets, Concurrent) and `zlib` for `x64-windows-static`.
The build uses vcpkg's installed packages directly (classic mode). Everything is linked statically
(Qt, zlib and the CRT), so the result is a single exe that only uses Windows system DLLs.
`scripts\vsenv.cmd` sets up the x64 developer environment; it also repairs a PATH containing a
stray `"`, or an inherited x86 environment, either of which breaks `vcvars64.bat`.

## Use

- **Opening files:** File > Open (multiple files), drag and drop, or the command line. Each
  file gets a tab. Opening a file that is already open switches to its tab.
- **Layer list (right pane):** the top-most layer is first and groups are folders. Groups
  start expanded or collapsed the way the file saved them. The icons on the right are the
  clipping-mask arrow (Photoshop's ↓) and an eye if the layer is visible; both only display
  state. The italic **preview** row at the bottom is the flattened image stored in the file.
  The program never composites layers itself.
- **Viewing:** selecting a layer (by click or with the arrow keys) shows its pixels cropped to
  the layer's bounds, as a standalone picture over a checkerboard, scaled to fit. Selecting a
  group leaves the canvas as it is. Shift and Ctrl select several rows; the canvas shows the
  last one clicked.
- **Moving:** drag the selected layers and groups. Dropping onto a group puts them at the top
  of that group; dropping between rows inserts them there (also out of a group, or between
  groups); dropping on the empty area puts them at the bottom of the list. Hovering over a
  collapsed group while dragging opens it. The "preview" row always stays last. Moving doesn't
  change any layer's flags: a clipped layer stays clipped (to whatever is now below it), and
  the file's stored flattened preview isn't recomputed.
- **Fit modes:** View > Fit Whole Image (Ctrl+1) or Fit Width (Ctrl+2). Fit Width scrolls
  with the mouse wheel or the scroll bar.
- **Renaming:** press F2 or double-click. Enter commits and Esc cancels. Renamed items are
  shown in bold and the tab gets a `*` (moves mark it too). An empty name is refused. Renaming
  back to the original, or moving layers back where they were, counts as unchanged.
- **Saving:** Ctrl+S saves in place: the new file is written to a temp file next to the
  original, re-parsed and checked, then swapped in atomically with `ReplaceFileW`. Save As
  writes elsewhere. If the file changed on disk since it was opened, the save is refused.
  No backup copy is kept. A rename still being typed is committed first.
- **Closing:** closing a tab (its X or Ctrl+W) or the app (window X, Alt+F4, File > Exit) with
  unsaved renames or moves asks Save / Discard / Cancel, listing what changed. Cancel, or a
  failed save, keeps the tab open.
- **Layout:** the canvas is 400 px and the list 200 px. Resizing the window only resizes the
  canvas, and the splitter can be dragged.

Display covers RGB and Grayscale, 8 and 16-bit, PSD and PSB, with raw, RLE, ZIP and
ZIP-with-prediction channels. 16-bit is shown by taking the high byte of each sample. For CMYK,
Lab, Indexed, 32-bit and other modes the canvas reports "not supported", but renaming still works.

## How it works

**Parsing** (`src/psd`, no Qt) reads the header, the version-info resource (`hasRealMergedData`)
and every layer record. For each record it keeps the byte positions of the name field, the
`luni` block, the extra-data length and each channel's data. The records come from the layer
info, or from the `Lr16`/`Lr32` block where Photoshop puts them in 16/32-bit documents. PSB's
8-byte length fields are handled. Pixel data is not read when a file is opened.

**Saving** (`PsdWriter`) works as follows:

- **Unchanged parts:** the header, colour mode data, image resources, unrenamed records, all
  channel data, the global layer info and the composite are copied in large chunks.
- **Renamed records:** the Pascal name, the `luni` block (added right after the name if the
  record has none) and the extra-data length are rewritten. Everything else in the record is
  copied.
- **Length fields:** the layer-and-mask section length and the layer container length
  (layer info, or `Lr16`/`Lr32`) change by the size difference.
- **Alignment:** each record's size change is kept a multiple of 4 through the `luni`
  padding, so all padding after it is copied unchanged.
- **Moves:** a group is stored as its record, its children and a hidden end-marker record,
  bottom-most first, and every layer's channel data follows the records in the same order. The
  new order comes from walking the tree. Records and their channel data blocks are written in
  that order, each copied unchanged. Three image resources list layers by position and are
  permuted in place so they keep pointing at the same layers: 1024 (target layer), 1026 (layer
  group IDs) and 1072 (layer-group-enabled IDs). If a group lacks its end marker (or an end
  marker lacks its group), moving is disabled for that file.

Photoshop, GIMP and most other readers use the Unicode name in `luni`. The legacy Pascal name is
encoded in a guessed code page:

1. If the file has `luni` names, use the code page that decodes the existing Pascal names to
   their `luni` text.
2. Otherwise, if every name is valid UTF-8, use UTF-8 (as Notepad++ does).
3. Otherwise, use the system code page.

**Display** (`src/gpu`, `shaders`) uses one D3D12 device and queue. When you select a layer, a
worker thread reads its compressed channels straight into an upload buffer (ZIP is inflated on
the CPU). The GPU then unpacks PackBits or raw rows into planes and merges them into
premultiplied RGBA8, which is the one format the renderer accepts. The kernels are adapted from
`../PsdPreview/native/src/gpu_d3d12.hlsl`. Only the RGBA image is kept.

The canvas is a native child window with a flip-model swap chain. Fitting the image is
PsdPreview's antialiased cubic resize, followed by one draw that composites over the
checkerboard. Window moves and overlaps never redraw it. Resizes, fit changes and scrolling
redraw on the GPU only.

**Releasing GPU memory:** selecting another layer frees the previous image at once. Each tab
keeps its own image when you switch tabs, and closing a tab frees it.

Diagnostics:

- `PSE_D3D12_DEBUG=1` prints D3D12 debug-layer messages on stderr (needs Windows' Graphics
  Tools feature).
- `PSE_WARP=1` uses the software rasterizer.
- `QT_FORCE_STDERR_LOGGING=1` sends Qt's own messages to stderr.

## Tests

The `build\x64-release\` tools are built along with the editor:

```
psdcheck dump <file>                               structure and names
psdcheck edit <in> <out> [--order i,j,...] [<index>=<name> ...]
                                                   write an edited copy + self-check; --order is the
                                                   new record order, bottom first, as original indices
decodetest <file> <record|-1> <out.pam>            GPU decode -> premultiplied RGBA8

python -I tests\verify_save.py <orig> <saved> [--order i,j,...] [<index>=<name> ...]
    independent check: every byte outside the renamed name fields is identical, records and
    their channel data are in the expected order, resources 1024/1026/1072 follow it
python -I tests\compare_decode.py build\x64-release\decodetest.exe <file> [--tolerance 2]
    every layer + composite against ImageMagick (16-bit: tolerance 2, high byte vs rounding)
python -I tests\make_zip_predicted.py <out.psd> <expected.pam> 8|16 [lr16]
    ZIP-with-prediction layers (optionally in an Lr16 block) with their expected pixels
python -I tests\make_groups.py <out.psd>
    nested groups plus resources 1024/1026/1072, for testing moves
```
