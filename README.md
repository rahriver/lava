# 🔥 LAVA

<img width="2145" height="1230" alt="g145" src="https://github.com/user-attachments/assets/0af584bc-c00e-4f6d-9777-680d35644359" />


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

## Preview

<img width="3440" height="1440" alt="image" src="https://github.com/user-attachments/assets/3414dae3-eb61-4014-94b6-21b1a9b7209e" />

<img width="1631" height="997" alt="image" src="https://github.com/user-attachments/assets/f5911a58-6af4-43fc-b6b7-2af0fea1b7cb" />

https://github.com/user-attachments/assets/4cd0ddc6-d330-4c36-92e3-e30ca0b26ec9
