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

- **Toolbar** (across the top) — Load, Save, Revert, Show all devices, Refresh live
- **Left panel** — the scrolling list of audio devices and their current levels
- **Right panel** — the name, type, percentage, and step buttons for the selected device
- **Status bar** (across the bottom) — shows what the program last did

---

## Adjusting a Level

1. Click on a device in the list on the left to select it.
2. The device name, type, and current level appear on the right.
3. Use the four buttons to adjust the level in precise steps:
   - **− −** decreases by 5%
   - **−** decreases by 1%
   - **+** increases by 1%
   - **+ +** increases by 5%
4. Each button press takes effect immediately and the percentage display updates.

---

## The Device List

By default, only devices that are set below 100% are shown, since these are
the ones most likely to need adjustment.

The buttons for managing the list are in the toolbar at the top of the window:

- Press **Show all devices** to reveal everything, including devices at full level.
- Press **Show active only** to return to the filtered view.
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

The program remembers the last file you used and will load it automatically
the next time you start.

---

## Reverting Changes

If you have adjusted some levels since loading or saving a file and want to
go back to what the file contained, press **Revert**.

This button is only available when:
- A file has been loaded or saved in the current session, **and**
- You have made at least one change since then

Revert is not available after **Refresh live**, because in that case there is
no saved reference point to go back to.

> **Note:** Revert goes back to the last loaded or saved *file*, not to the
> levels that were set when you started the program.

---

## The Status Bar

The strip along the bottom of the window shows brief messages:

| Message | Meaning |
|---|---|
| ⚠ Unsaved changes | Levels have been adjusted since the last Save or Load |
| Saved. | The current levels have been written to a file |
| Loaded. | A file has been read and its levels applied |
| Reverted to saved levels. | Levels restored from the last file |
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
