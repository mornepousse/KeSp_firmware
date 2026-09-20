# Tamagotchi Sprite Guide — KaSe Keyboard

## Technical constraints

| Parameter | Value |
|-----------|--------|
| **Sprite size** | 32×32 pixels |
| **Colors** | Monochrome (1 bit: pixel ON or OFF) |
| **Round screen** | 240×240 px — sprite displayed at ~80×80 px (2.5x zoom) |
| **OLED screen** | 128×64 px — sprite displayed at 32×32 px (native size) |
| **Available area** | ~100×100 px at the center of the round screen |
| **Delivery format** | PNG 32×32 monochrome (pure black and white, no gray) |

## What to draw

### 20 creatures (evolution levels)

Each creature has **2 poses**:
- **Main**: normal / active pose
- **Idle**: rest pose (slight variation — closed eyes, relaxed posture, etc.)

The animation toggles between the 2 poses every 600ms plus a slight vertical breathing motion.

| Level | Suggested name | Style | Description |
|--------|-------------|-------|-------------|
| 1 | Egg | Simple | An egg with a pattern |
| 2-5 | Baby | Small, round | Small cute creature, simple |
| 6-10 | Child | More detailed | Visible limbs, expression |
| 11-15 | Teen | Complex | Distinct personality |
| 16-19 | Adult | Detailed | Unique, elaborate design |
| 20 | Ultimate | Epic | Final form, impressive |

### 8 status icons (16×16 pixels)

| Icon | Use |
|-------|-------|
| Food | Hunger bar |
| Heart | Happiness |
| Lightning bolt | Energy |
| Medical cross | Health/medicine |
| Star | Level/XP |
| Zzz | Sleep |
| ! | Alert (low hunger) |
| ☺ | Celebration |

## Design rules

### Monochrome pixel art
- **Black and white only** — no gray levels
- The firmware colorizes the sprite according to emotional state:
  - White = idle
  - Green = happy
  - Yellow = excited
  - Orange = eating
  - Red = sick
  - Blue = sad
  - Dark purple = sleeping
  - Magenta = celebrating

### Tips
- **Center** the creature in the 32×32 frame
- **Keep a 2-3px margin** on the edges (zoom can crop the outermost pixels)
- **Eyes** matter a lot — they carry the emotion
- The difference between main and idle should be **subtle** (1-3 pixels of change) — the animation is fast (600ms)
- **Clear silhouette** — the sprite must be recognizable even at 32×32

### Examples of main/idle difference
```
Main:           Idle:
  ●  ●            —  —       (open eyes → closed)
  ╰──╯            ╰──╯

   /\              /\
  /  \            /  \        (same body, slight rotation)
 /    \          /   \
```

## Delivery

### File format
```
sprites/
├── egg_main.png      (32×32, black and white)
├── egg_idle.png
├── baby1_main.png
├── baby1_idle.png
├── ...
├── ultimate_main.png
├── ultimate_idle.png
├── icon_food.png     (16×16)
├── icon_heart.png
├── icon_bolt.png
├── icon_meds.png
├── icon_star.png
├── icon_sleep.png
├── icon_alert.png
└── icon_celebrate.png
```

### Validation
- Open in a pixel art editor (Aseprite, Piskel, GIMP)
- Check that it is indeed 32×32 (or 16×16 for icons)
- Check that there are only 2 colors (pure black #000000 and pure white #FFFFFF)
- The background must be **black** (#000000), the creature in **white** (#FFFFFF)

## Recommended tools

| Tool | URL | Use |
|-------|-----|-------|
| **Piskel** | piskelapp.com | Free, online, perfect for pixel art |
| **Aseprite** | aseprite.org | Pro, paid, the best for animation |
| **GIMP** | gimp.org | Free, for final touch-ups |
| **Lospec** | lospec.com/pixel-art-tutorials | Pixel art tutorials |

## Context

The sprite is displayed in the center of a round smartwatch-style screen (240×240 px). It is surrounded by:
- A KPM arc (typing speed) around the edge
- The keyboard layout name at the top
- Stat bars (hunger, happiness, energy) below
- The creature's level and name as text

The tamagotchi lives off keyboard use — the more you type, the happier it is. It evolves by earning XP from keystrokes.
