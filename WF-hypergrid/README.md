# Animated Tiling Layout for Wayfire

A Hyprland-inspired animated tiling window manager plugin for Wayfire.

## Features

- **Smooth Animations**: Windows animate smoothly to their new positions when the layout changes
- **Dwindle Layout**: Binary tree tiling like Hyprland's dwindle layout
- **Configurable Bezier Curves**: Customize the animation easing just like in Hyprland
- **Automatic Tiling**: New windows are automatically tiled
- **Gap Support**: Configurable gaps between windows

## How It Works

Unlike traditional tiling in Wayfire, this plugin doesn't just snap windows to their positions. Instead:

1. When a new window opens or the layout changes, the plugin calculates the new positions
2. Each window's position and size are animated using bezier-curve easing
3. View transformers are used to show the windows at their animated positions
4. When animation completes, the final geometry is applied

This creates the smooth, fluid tiling experience that Hyprland is known for.

## Installation

```bash
# Build
meson setup build --prefix=/opt/wayfire  # or your wayfire prefix
meson compile -C build
sudo meson install -C build

# Enable in wayfire.ini
# [core]
# plugins = ... animated-tile
```

## Configuration

Add to your `~/.config/wayfire.ini`:

```ini
[animated-tile]
# Automatically tile new windows
tile_by_default = true

# Gap between windows (pixels)
gap = 10

# Animation duration (milliseconds)
duration = 300

# Bezier curve control points (Hyprland default)
bezier_p1_x = 0.0
bezier_p1_y = 0.75
bezier_p2_x = 0.15
bezier_p2_y = 1.0

# Keybindings
toggle_tile = <super> KEY_T
focus_left = <super> KEY_H
focus_right = <super> KEY_L
focus_up = <super> KEY_K
focus_down = <super> KEY_J
```

## Bezier Curve Examples

The bezier curve controls how the animation accelerates/decelerates:

| Style | P1 (x,y) | P2 (x,y) | Description |
|-------|----------|----------|-------------|
| Hyprland Default | (0.0, 0.75) | (0.15, 1.0) | Fast start, smooth ease-out |
| Linear | (0.25, 0.25) | (0.75, 0.75) | Constant speed |
| Ease-in-out | (0.42, 0.0) | (0.58, 1.0) | Slow start and end |
| Bouncy | (0.68, -0.55) | (0.27, 1.55) | Overshoots slightly |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Animated Tiling Plugin                    │
├─────────────────────────────────────────────────────────────┤
│  TileTree                                                    │
│  ├── TileNode (split: horizontal)                           │
│  │   ├── TileNode (leaf: window A)                          │
│  │   │   └── AnimatedGeometry                               │
│  │   └── TileNode (split: vertical)                         │
│  │       ├── TileNode (leaf: window B)                      │
│  │       │   └── AnimatedGeometry                           │
│  │       └── TileNode (leaf: window C)                      │
│  │           └── AnimatedGeometry                           │
│  └── BezierCurve (shared config)                            │
│                                                              │
│  Animation Loop                                              │
│  └── Per frame: tick animations → apply geometry            │
└─────────────────────────────────────────────────────────────┘
```

## TODO / Future Features

- [ ] Resize tiled windows with mouse
- [ ] Move windows between tiles
- [ ] Multiple layout modes (master-stack, grid, etc.)
- [ ] Per-workspace layouts
- [ ] Window rules (float certain apps)
- [ ] Rounded corners on tiles
- [ ] Border/decoration support

## Credits

Inspired by:
- [Hyprland](https://github.com/hyprwm/Hyprland) - Animation system and dwindle layout
- [Swayfire](https://github.com/Javyre/swayfire) - Wayfire tiling plugin architecture
- [Wayfire](https://github.com/WayfireWM/wayfire) - The compositor

## License

MIT
