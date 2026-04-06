# CYD Chore Tracker

A touchscreen chore tracker for kids, built on the ESP32 Cheap Yellow Display (CYD) and powered by Todoist.

Kids check off tasks on the device, and everything syncs back to your Todoist project. Completing all tasks reveals a daily reward.

## Features

- **Tabbed sections** — organize tasks by Morning, Night, Weekly (or any custom sections)
- **Touch checkboxes** — tap anywhere on a task row to check it off
- **Progress bar** — rainbow progress bar tracks completion per section
- **Daily rewards** — random reward from a Todoist "Rewards" section, consistent per day
- **Weekly rewards** — configurable reward text for completing weekly chores
- **Celebration screen** — emoji art and personalized praise when all tasks are done
- **Smart filtering** — daily tasks show only today's, weekly tasks show this week's
- **Recurring tasks** — tasks automatically reappear on schedule (daily/weekly)
- **Reset button** — settings tab lets you reset all tasks for testing
- **Auto-sleep** — screen dims after 5 min, turns off after 10 min, tap to wake
- **Dark theme** — pink/purple accent colors with rainbow task rows

## Hardware

- **ESP32 CYD** (ESP32-2432S028R or similar 2.8" ILI9341 variant)
  - Tested on SKU: E32R28T (ESP32-E 2.8inch)
- USB-C cable (data, not charge-only)
- CH340 USB driver (macOS: `brew install --cask wch-ch34x-usb-serial-driver`)

## Setup

### 1. Install PlatformIO

```bash
pipx install platformio
```

### 2. Configure secrets

Copy the example and fill in your values:

```bash
cp include/secrets.h.example include/secrets.h
```

Edit `include/secrets.h`:

| Setting | Description |
|---------|-------------|
| `WIFI_SSID` | Your 2.4GHz WiFi network name |
| `WIFI_PASSWORD` | WiFi password |
| `TODOIST_API_TOKEN` | From todoist.com > Settings > Integrations > Developer |
| `TODOIST_PROJECT_ID` | The alphanumeric ID from your project URL |
| `USER_NAME` | Child's name (shown in praise messages) |
| `TIMEZONE_OFFSET` | UTC offset in hours (e.g., -7 for PDT) |
| `DST_OFFSET` | Daylight saving (0 or 1) |
| `WEEKLY_REWARD` | Reward text for weekly chores (e.g., "$10") |

### 3. Set up Todoist project

Create a project with these sections:

- **Morning** — daily recurring tasks (e.g., "Brush teeth", due: "every day")
- **Night** — daily recurring tasks (e.g., "Read for 15 min", due: "every day")
- **Weekly** — weekly recurring tasks (e.g., "Clean room", due: "every monday")
- **Rewards** — uncompletable reward options (prefix with `* `, e.g., "* Extra screen time")

Section names are flexible — any section with "week" or "chore" in the name is treated as weekly. Everything else is treated as daily.

### 4. Build and flash

```bash
pio run --target upload --upload-port /dev/cu.usbserial-XXX
```

Replace `XXX` with your serial port (check with `ls /dev/cu.usbserial*`).

## Todoist API

Uses the [Todoist REST API v1](https://developer.todoist.com/api/v1/) (`api.todoist.com/api/v1`).

## Libraries

- [LVGL](https://lvgl.io/) v9 — UI framework
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch input
- [ArduinoJson](https://arduinojson.org/) — JSON parsing
- [Noto Emoji](https://github.com/googlefonts/noto-emoji) — emoji pixel art (SIL OFL license)

## License

MIT
