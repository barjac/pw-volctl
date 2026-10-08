# PipeWire Volume Control — User Guide

## What is it?

**pw-volctl** is a simple audio level control tool for Linux systems using PipeWire.
It lets you adjust the volume of individual audio devices — microphones, speakers,
radio interfaces, and so on — without using the system sound widget, which can
sometimes interfere with specialist audio setups.

You can also save your preferred level settings to a file and reload them whenever
you need them, making it easy to switch between different configurations (for
example, one set of levels for 40m operation, another for 80m).

---

## The Layout

The window has four areas:

- **Toolbar** (across the top) — Load, Save, Revert, Mode (% or dB), Showing All / Active Only, Refresh live
- **Left panel** — the scrolling list of audio devices and their current levels
- **Right panel** — the name, type, percentage, and step buttons for the selected device
- **Status bar** (across the bottom) — shows what the program last did

---

## Adjusting a Level

1. Click on a device in the list on the left to select it.
2. The device name, type, and current level appear on the right.
3. Use the four buttons to adjust the level in precise steps. What a step
   is depends on the **Mode** button in the toolbar:

   | Button | Mode: % | Mode: dB |
   |---|---|---|
   | **− −** | −5% | −3 dB |
   | **−** | −1% | −0.1 dB |
   | **+** | +1% | +0.1 dB |
   | **+ +** | +5% | +3 dB |

   Hold a button down to repeat the step.
4. Each button press takes effect immediately and the level display updates.
   Levels are set and read back exactly, so a level set in dB reads back as
   the same dB value.

---

## The Device List

By default all devices are shown (**Showing All**).

The buttons for managing the list are in the toolbar at the top of the window:

- Press **Showing All** to switch to **Active Only**, which hides devices at
  exactly 100% (0 dB) so only adjusted ones are listed. Press it again to show
  everything.
- Press **Refresh live** to re-read the current levels from the system.
  Use this if something outside the program has changed a level.

---

## Saving and Loading Presets

You can save the current set of levels to a named file and reload it later.
This is useful if you use the same computer for different activities that need
different audio levels.

### Saving
Press **Save**. A file chooser will appear. Give the file a meaningful name
(for example `40m.db` or `podcast.db`) and press Save.

The window title will update to show the name of the file you saved to.

### Loading
Press **Load**. A file chooser will appear. Select the file you want and
press Open. All levels in the file will be applied immediately.

The program always starts from the live levels; it does not load a file
automatically. It remembers the last file you used and opens the file chooser
there next time.

---

## Reverting Changes

If you have adjusted some levels and want to go back, press **Revert**. It
returns every device to the *reference* levels, which are taken:

- when the program starts (the live levels at that moment),
- when you press **Refresh live**,
- when you load or save a file.

So if you change, say, the mic level and forget what it was, Revert puts it
back to where it was when you opened the program. Revert is available whenever
there are changes since the reference.

---

## The Status Bar

The strip along the bottom of the window shows brief messages:

| Message | Meaning |
|---|---|
| ⚠ Unsaved changes | Levels have been adjusted since the reference (start, Refresh live, Load or Save) |
| Saved. | The current levels have been written to a file |
| Loaded. | A file has been read and its levels applied |
| Reverted. | Levels restored to the reference |
| Refreshed from live. | Levels re-read from the system |

---

## Closing the Program

If you have made adjustments that have not been saved to a file, you will be
asked whether you really want to close. Choose:

- **Cancel** — go back to the program (so you can save first if you wish)
- **Close anyway** — close without saving; the live audio levels stay as
  you left them, but no file is updated

> The audio levels you set remain active after closing — the program is just
> a control panel. Closing it does not reset anything.

---

## Converting a dB Change to a Percentage

The percentage shown for each device isn't a straight (linear) fraction of full
volume — it follows the same curve PulseAudio-style volume sliders use, so a
given percentage step means a different dB change depending on where you start.

```
percent = 100 x 10^(dB / 60)
dB      = 60 x log10(percent / 100)
```

100% is always 0 dB (unity gain). Some reference points:

| dB change | Percentage |
|---|---|
| 0 dB | 100% |
| −6 dB | 79% |
| −10 dB | 68% |
| −18 dB | 50% |
| −20 dB | 46% |
| −30 dB | 32% |
| −40 dB | 22% |
| −60 dB | 10% |

> **Note:** the same percentage-point step is a much bigger dB change lower
> down the scale than near 100% — e.g. a −5% adjustment near 100% is only
> about −1.3 dB, but the same −5% near 50% is about −2.8 dB. If you're
> adjusting two pw-volctl-controlled stages in series to hit a known dB
> change, it pays to park one control at a round percentage (ideally 100%,
> where the table above is exact) and do the fine adjustment entirely on the
> other, rather than adding up two curved conversions in your head.

---

## File Location

Preset files can be stored anywhere, but by default the file chooser opens in:

```
~/.config/pipewire/
```

This is also where the program keeps its temporary working file (`pw-volctl.tmp`)
and the record of the last file you used (`pw-volctl.last`).
