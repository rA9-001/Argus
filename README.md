# Argus

A screenshot tool for Windows. One 308 KB executable, no installer, no runtime,
no background CPU. Press a key, drag a box, hit Enter.

*Argus Panoptes, the hundred-eyed watchman of Greek myth, who never closed more
than half his eyes at once.*

Built to keep Lightshot's flow — the thing it gets right is that a capture is
*drag, mark up, done* without a window ever opening — while being smaller and
faster than either Lightshot or Greenshot, and without an account or an uploader.

## Install

[**Download `argus.exe`**](https://github.com/rA9-001/Argus/releases/latest) —
put it wherever you want it to live and run it. It sits in the tray.

There is nothing else to install. It links only against DLLs that ship with
Windows — no Visual C++ redistributable, no .NET.

**It starts with Windows from the first run.** Argus adds itself to
`HKCU\...\CurrentVersion\Run` pointing at wherever you put the exe, and keeps
that entry in step on every launch — so moving the exe and running it once is
enough to fix the shortcut. Turn it off from the tray icon → **Run at startup**,
or set `runAtStartup=0` in the settings file. Nothing is written outside your
own user account, and no service or scheduled task is created.

> Windows SmartScreen will warn about the download, because the binary is not
> code-signed — a certificate costs more per year than this project is worth.
> *More info → Run anyway*, or build it yourself in ten seconds; see
> [Building](#building).

> On Windows 11 new tray icons are hidden in the overflow chevron by default.
> Drag it onto the taskbar to keep it visible.

## Hotkeys

| Key | Action |
| --- | --- |
| `PrintScreen` | Select a region |
| `Ctrl+PrintScreen` | Grab the active window |
| `Shift+PrintScreen` | Grab the monitor under the cursor |
| `Ctrl+Shift+PrintScreen` | Repeat the last region |

All four are configurable - see [Settings](#settings). Clicking the tray icon
always starts a region capture.

If another tool already owns a key (Greenshot, Lightshot and ShareX all grab
`PrintScreen`), Argus says so in a tray notification at startup and keeps
running; pick a different key, or close the other tool.

### Windows 11 takes PrintScreen for itself

On Windows 11 the PrintScreen key opens the built-in Snipping Tool, and it does
so inside the input stack - *before* any registered hotkey is consulted. This is
on by default, and `RegisterHotKey` still reports success, so an app cannot tell
it lost the key by asking. Argus detects the setting directly and offers to take
the key over: **tray icon -> Give the PrintScreen key to Argus**, or click the
notification it shows at startup.

That flips the same user setting as *Settings -> Accessibility -> Keyboard ->
Use the Print screen key to open screen capture*
(`HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled`). Windows only
re-decides who owns the key when a **newly started** process claims it, so Argus
restarts itself to apply the change - re-registering in place is not enough.
Handing the key back to Windows is not symmetric: Windows arms its handler at
sign-in, so it reclaims PrintScreen only after you next sign in.

## While capturing

The screen freezes, dims, and a magnifier follows the cursor showing the exact
pixel and its hex colour.

| | |
| --- | --- |
| Drag | Select a region |
| Click | Grab the window or pane under the cursor |
| Drag edges / corners | Resize; drag inside to move |
| `Ctrl+A` | Select the whole monitor |
| Arrows | Nudge 1px — `Shift` for 10px, `Ctrl` to resize |
| `C` | Copy the hex colour under the cursor and exit |
| `M` | Toggle the magnifier |
| `Esc` / right-click | Back out one step, then cancel |

## Marking up

| | | | |
| --- | --- | --- | --- |
| `V` Select | `P` Pen | `L` Line | `A` Arrow |
| `R` Rectangle | `E` Ellipse | `H` Highlighter | `T` Text |
| `N` Numbered step | `B` Pixelate | `1`–`6` Colour | Wheel: thickness |

`Ctrl+Z` / `Ctrl+Y` undo and redo. Annotations are vectors until you export, so
undo is exact and the saved image is never re-compressed.

## Finishing

| | |
| --- | --- |
| `Enter` or double-click | Copy to clipboard and close |
| `Ctrl+S` | Save as… |
| `Ctrl+Shift+S` | Save straight to the screenshots folder |
| `Esc` | Cancel |

Copies land on the clipboard as PNG, `CF_DIBV5` and `CF_DIB`, so they paste
correctly into browsers, chat apps and Office alike. Saves go to
`Pictures\Screenshots\Screenshot YYYY-MM-DD HH.MM.SS.png` by default.

## Settings

`%APPDATA%\Argus\argus.ini`, created and commented on first run. Tray icon →
**Edit settings…** opens it. Restart Argus after editing.

It covers the four hotkeys, the save folder, whether `Ctrl+S` skips the file
dialog, the magnifier, the dim strength, the accent colour, and whether Argus
starts with Windows.

## Command line

| | |
| --- | --- |
| `argus.exe` | Start in the tray |
| `argus.exe --region` / `--window` / `--full` / `--repeat` | Capture now |
| `argus.exe --quit` | Close the running instance |
| `argus.exe --bench` | Time the capture pipeline |
| `argus.exe --trace` | Log hotkey-to-visible latency to %TEMP% |

A second launch hands its request to the instance already running, so you can
bind `argus.exe --region` to any key your keyboard software or the Start menu
can reach.

## Speed

`argus.exe --trace` writes a breakdown of the real hotkey-to-visible path into
your `%TEMP%` folder as `argus-trace.txt`. On a 3840×1080 two-monitor desktop:

| Step | Time |
| --- | --- |
| Capture the whole desktop | 30 ms |
| Find window snap targets | 0.1 ms |
| Create the overlay window | 1 ms |
| Show it | 5 ms |
| First paint | 9 ms |
| **Press to visible** | **~46 ms** |

The capture dominates, and it is almost entirely GPU-to-RAM readback: about
170 Mpx/s, against roughly 1 Gpx/s for the same blit between two memory
bitmaps. Neither a device-dependent destination nor per-monitor device
contexts moved it — both were measured, and both came out slightly slower.
Getting past that number means leaving GDI for the Desktop Duplication API.

The first paint used to cost 26 ms, because the base layer was assembled in
three passes over the frame: copy the capture into the back buffer, then
read-modify-write the whole thing with `AlphaBlend` to dim it. It is now a
single pass that reads the capture and writes the dimmed result straight into
the back buffer, with the blend reduced to three table lookups per pixel.
Composing directly into the window DC was tried too, and was both slower and
far less consistent — every operation then goes through the compositor's
surface.

Encode and clipboard costs land after you press Enter, so they never sit
between you and the selection.

## How it works

- **One frozen frame.** The desktop is captured into a top-down 32bpp DIB up
  front. Everything after that paints from that copy, so no application below
  ever has to redraw and the image can't change under you mid-selection.
- **Damage-tracked repaints.** Moving the mouse invalidates the magnifier, two
  1px crosshair strips, and the XOR of the old and new selection rectangles —
  not the screen. That is what keeps dragging smooth at 4K.
- **Text is drawn with GDI, not GDI+.** GDI+ lays glyphs out in float space
  with grayscale coverage, which reads soft next to native Windows UI. Chrome
  goes through GDI with ClearType so stems are hinted onto the pixel grid;
  image content - the text tool and the step numbers - uses grayscale instead,
  because subpixel colour fringes are wrong in a file that gets scaled or
  viewed on another display.
- **Vector annotations.** Shapes are stored as geometry and re-rendered each
  frame with GDI+ anti-aliasing. Undo is a pop; export is lossless.
- **Export re-renders from the pristine capture**, so dimming, handles, the
  toolbar and the magnifier can never leak into the saved image.
- **Per-monitor-v2 DPI aware.** Coordinates are physical pixels throughout, so a
  selection can span monitors and still be pixel-exact.
- GDI does the compositing (plain blits, which are fast); GDI+ is used only for
  the anti-aliased chrome and annotations, over the small area they cover.

## Building

Needs Visual Studio with the **Desktop development with C++** workload
(any recent version — the script finds it).

```
build.cmd            release  -> build\argus.exe
build.cmd debug      unoptimised, with symbols
build.cmd clean
```

Warnings are errors (`/W4 /WX`). The CRT is linked statically so the output has
no redistributable dependency.

`res\make_icon.ps1` regenerates `res\app.ico`; the generated icon is committed,
so a normal build does not need PowerShell.

### Layout

```
src\app.h        shared types, settings, and the public surface of each module
src\main.cpp     tray icon, global hotkeys, single-instance, entry point
src\capture.cpp  screen capture, window/pane snap-target discovery
src\overlay.cpp  overlay window, input, damage tracking, commands
src\paint.cpp    dimming, annotations, toolbar, magnifier, export rendering
src\output.cpp   PNG encoding (WIC), clipboard, save dialog
src\util.cpp     paths, INI settings, hotkey parsing
res\             icon, manifest, version resource
```

## Deliberately not included

Cloud upload and short links, scrolling capture, video or GIF recording, OCR,
plugins, auto-update, telemetry. Each of those is what turns a screenshot tool
into a background service.

## Known limitations

- Exclusive-fullscreen games and DRM-protected windows can capture as black.
  That is a GDI limitation; capturing them would require the Desktop
  Duplication API and a Direct3D device.
- The text tool takes direct keystrokes and does not support IME composition,
  so it is not usable for Chinese, Japanese or Korean input.
- On a mixed-DPI multi-monitor setup, toolbar and badge *sizing* follows the
  monitor under the cursor while their *placement* follows the selection, so a
  toolbar for a selection on another monitor can be slightly off-size.

## License

MIT. See [LICENSE](LICENSE).
