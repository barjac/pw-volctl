# pw-volctl

A small GTK4 volume control panel for PipeWire, built around libpulse's
PipeWire-pulse compatibility layer. Lets you adjust individual device
levels in precise 1%/5% steps, see a live peak level meter per device, and
save/load whole sets of levels as named presets — handy for switching
between different audio setups (e.g. different radio bands, or mic vs.
line-in configurations) without digging through the system volume widget.

See [`pw-volctl-guide.md`](pw-volctl-guide.md) for full usage instructions.

## Dependencies

- GTK4
- libpulse (PulseAudio client library) and its GLib main-loop binding
- A C compiler (gcc) and `pkg-config`

### Debian / Ubuntu
```
sudo apt install build-essential pkg-config libgtk-4-dev libpulse-dev
```

### Fedora
```
sudo dnf install gcc pkgconf-pkg-config gtk4-devel pulseaudio-libs-devel
```

### Mageia
```
sudo urpmi gcc pkgconf lib64gtk4.0-devel lib64pulseaudio-devel
```

## Building

```
make
```

This builds the `pw-volctl` binary in the current directory.

To install it to `~/.local/bin`:

```
make install
```

Make sure `~/.local/bin` is on your `PATH`.

To clean up build output:

```
make clean
```

## Desktop integration (optional)

A `.desktop` file and icon are included (`pw-volctl.desktop`, `pw-volctl.svg`).
After `make install`, copy these into your local applications/icons
directories to get a launcher entry, e.g.:

```
mkdir -p ~/.local/share/applications ~/.local/share/icons
cp pw-volctl.desktop ~/.local/share/applications/
cp pw-volctl.svg ~/.local/share/icons/
```
