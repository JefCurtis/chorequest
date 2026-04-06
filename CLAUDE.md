# CLAUDE.md

## Project Overview

CYD Chore Tracker — an ESP32-based touchscreen chore list for kids that syncs with Todoist. Built for the Cheap Yellow Display (CYD) hardware.

## Tech Stack

- **MCU**: ESP32 (ESP32-2432S028R / E32R28T)
- **Display**: 2.8" ILI9341, 320x240, resistive touch (XPT2046)
- **Framework**: Arduino via PlatformIO
- **UI**: LVGL v9
- **API**: Todoist REST API v1 (`api.todoist.com/api/v1`)

## Build & Flash

```bash
pio run --target upload --upload-port /dev/cu.usbserial-XXX
```

Upload baud: 460800 (921600 causes errors on this board).

## Key Files

- `src/main.cpp` — all application code (single file)
- `include/secrets.h` — WiFi, Todoist, and personalization config (gitignored)
- `include/secrets.h.example` — template for secrets
- `include/lv_conf.h` — LVGL configuration
- `include/emoji_art.h` — pixel art emoji images as C arrays (generated from PNGs in `assets/`)
- `platformio.ini` — board config, TFT_eSPI pin mappings, LVGL settings

## Hardware Notes

- Display uses `ILI9341_2_DRIVER` (not `ILI9341_DRIVER`) — text won't render with the wrong driver
- Font loading flags (`LOAD_GLCD`, `LOAD_FONT2`, etc.) are required or text is invisible
- Touch uses a SEPARATE SPI bus (HSPI) from the display — use `XPT2046_Touchscreen` library, not TFT_eSPI built-in touch
- Touch pins: CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36
- Display pins: MOSI=13, MISO=12, SCLK=14, CS=15, DC=2, BL=21
- Serial port may change between connections (check `ls /dev/cu.usbserial*`)
- CH340 USB driver required on macOS

## Architecture

Single-file app (`main.cpp`) with these logical sections:

1. **Hardware init** — TFT, touch (HSPI), LVGL display/input drivers
2. **WiFi & NTP** — connects to WiFi, syncs time for date filtering
3. **Todoist API** — fetch sections, fetch tasks, send completions, reset tasks
4. **UI (LVGL)** — tabbed interface with checkboxes, progress bars, celebration screens
5. **Main loop** — LVGL timer, completion queue processing, auto-refresh, sleep management

## Todoist Project Structure

Sections in the Todoist project:
- **Daily sections** (Morning, Night, etc.) — filtered to show only today's tasks, recur "every day"
- **Weekly sections** (any name containing "week" or "chore") — filtered to show this week's tasks, recur "every monday"
- **Rewards** section — uncompletable tasks (prefixed with `* `), shown as daily rewards on completion screen

## Git Rules

- Do NOT add co-author tags to commits
- Break commits into logical units (e.g., separate config, UI, API, assets)

## Conventions

- All personal config goes in `secrets.h`, never hardcoded
- Use `drawString()` not `print()`/`println()` with TFT_eSPI (print renders invisible text with ILI9341_2_DRIVER)
- LVGL widgets handle touch — no manual coordinate mapping needed
- API calls should not block the UI loop — queue completions and process one per iteration
- Use `WiFiClientSecure` with `setInsecure()` for HTTPS on ESP32
