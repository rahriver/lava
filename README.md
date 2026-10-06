# LAVA

**L**ayer **A**udio **V**isu**A**lizer — an audio visualizer drawn on your Wayland desktop:
above the wallpaper, below windows, and click-through.

Every style is a GPU shader (OpenGL ES 2) fed by [cava](https://github.com/karlstav/cava), so it uses a few percent
of one CPU core and stops drawing while windows cover it.

Styles: `bars` · `mountains` · `mirror` · `top` · `circle` · `wave`

## Requirements

- A Wayland compositor with the wlr-layer-shell protocol (Sway, Hyprland, river, niri, Wayfire, labwc, …)
- `cava` (with PipeWire input; it is started and stopped by lava)
- A GPU driver with EGL / OpenGL ES 2 (Mesa is fine)
- Build: `wayland`, `wayland-protocols`, `mesa`, `pkgconf`, a C compiler and `make`

## Install

```sh
make
sudo make install            # /usr/local/bin/lava   (or: make install PREFIX=~/.local)
```

## Usage

```sh
lava [-s px] [-f fps] [style]
```

| option | |
|---|---|
| `style` | `bars` (default), `mountains`, `mirror`, `top`, `circle`, `wave` |
| `-s px` | height in pixels (the circle's diameter); by default it scales with the screen height |
| `-f fps` | frame rate, 10–240 (default 60) |

Examples: `lava circle` · `lava -s 300 wave` · `lava -f 144 mirror`

Stop it with `pkill lava`. Run one instance at a time.

## Colors

lava uses the colors of [pywal](https://github.com/dylanaraps/pywal) when `~/.cache/wal/colors.json` exists,
and a built-in palette otherwise. After changing the wallpaper, recolor it live with `pkill -USR2 lava`.

## License

MIT
