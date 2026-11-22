# oh-wallpanel: Functional Specification (draft v0.1)

The owner's decisions are listed below. The defaults this draft proposed were all confirmed by the owner on 2026-11-21.
- Hardware: Waveshare ESP32-S3-Touch-LCD-4.3.
- Firmware: ESP-IDF + LVGL.
- openHAB link: REST + SSE, with no MQTT.
- Purpose: a wall controller.
- Screens are defined by openHAB tags. Items with the first tag form the home screen. Each further tag adds a screen.
- Controls: lights (switch and dimmer), scenes and "all off", climate, appliances, and a status line.

## 1. Scope

The panel is ESP-IDF firmware for the Waveshare ESP32-S3-Touch-LCD-4.3:
- ESP32-S3 (rev v0.2) with 8 MB flash and 8 MB octal PSRAM (measured on the owner's board; Waveshare also sells a 16 MB variant);
- 4.3" 800×480 RGB565 LCD;
- GT911 capacitive touch;
- CH422G I/O expander.

It:
- connects to openHAB over Wi-Fi;
- builds its screens from openHAB item tags;
- shows live item states;
- sends commands when tiles are touched.

All panel content is configured in openHAB, through tags and optional item metadata. Nothing about items is configured on the panel itself.

Out of scope for v1: cameras/video, charts, audio, more than one openHAB server.

## 2. Hardware

| Function | Connection |
|---|---|
| RGB LCD | HSYNC 46, VSYNC 3, DE 5, PCLK 7, D0–D15 = 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 |
| I2C (touch + expander) | SDA 8, SCL 9 |
| GT911 touch | I2C, address 0x5D (or 0x14) |
| CH422G expander | EXIO1 touch reset, EXIO2 backlight on/off, EXIO3 LCD reset, EXIO4 SD card CS, EXIO5 USB_SEL |

- **REQ-HW-1:** the board support is `components/wp_board`, vendored from `bobscott45/waveshare_esp32_s3_touch_lcd_4_3` 1.0.7 (IDF 6.x, LVGL 9.6). The framebuffer lives in PSRAM, with bounce-buffer mode to avoid display drift.
- **REQ-HW-4:** CH422G EXIO5 (USB_SEL) is always kept low. When it is high, GPIO19/20 are routed to the CAN transceiver and the native USB port disappears; the CH422G keeps that state across an ESP reset. The upstream BSP set it during touch reset, which is why it is vendored. All CH422G writes go through one shadow register.
- **REQ-HW-2:** the backlight can only be switched on and off, because the CH422G has no PWM. Dimming would need a hardware modification and is out of scope for v1.
- **REQ-HW-3:** all I2C transfers stay within the controller FIFO. This is the lesson from the sensor-actor project, where longer transfers corrupted the ESP-IDF 6.1 i2c_master driver state.

## 3. Screens from openHAB tags

- **REQ-SCR-1:** the panel configuration holds an ordered list of screens. Each entry is `tag` or `tag=Title`, for example `wp_home=Home, wp_kitchen=Kitchen, wp_outdoor=Outside`. The first entry is the home screen.
- **REQ-SCR-2:** a screen shows every item that has its tag, fetched with `GET /rest/items?tags=<tag>&metadata=wallpanel&fields=…`.
- **REQ-SCR-3:** a screen also shows every rule with the same tag, as scene buttons (`GET /rest/rules?tags=<tag>`).
- **REQ-SCR-4:** tiles are sorted by `wallpanel` metadata `order`, then by label.
- **REQ-SCR-5:** the screen list is reloaded:
  - at boot;
  - when the panel reconnects to openHAB;
  - every 10 minutes;
  - when the item or rule registry changes (SSE `ItemAddedEvent`, `ItemUpdatedEvent`, `ItemRemovedEvent`, and the same for rules). Re-tagging an item or rule in openHAB therefore changes the panel within seconds. Changing only `wallpanel` metadata publishes no event (openHAB 5.2), so it shows up with the 10-minute reload, or at once with the console command `reload`.
- **REQ-SCR-6:** you switch screens with a bottom tab bar showing the screen titles. Swiping left and right also works.
- **REQ-SCR-7:** after 60 s without a touch, the panel returns to the home screen.

### 3.1 Tile types

The tile type is chosen automatically from the item type and its semantic tags. The `wallpanel` metadata value can override it (for example `value="scene"`).

| Item | Default tile | Touch action |
|---|---|---|
| Switch | toggle tile (icon + label, highlighted when ON) | toggle ON/OFF |
| Dimmer | dimmer tile: toggle + brightness slider | tap toggles, slider sends 0–100 |
| Color | as Dimmer, using the brightness part of the HSB state | as Dimmer |
| Rollershutter | up / stop / down buttons + position | UP / STOP / DOWN |
| Number with Setpoint semantics | setpoint stepper (− value +) | ± step (default 0.5); min/max from the state description |
| Number / Number:\<dim\> (Measurement) | value tile (label, value, unit) | none |
| String / DateTime / Contact | value tile | none |
| Group (Switch/Dimmer base type) | toggle tile acting on the group | toggle ON/OFF |
| Rule with the screen tag | scene button | run the rule (`POST /rest/rules/<uid>/runnow`) |

- **REQ-TILE-1:** values are formatted with the state-description pattern, for `%d`, `%.Nf`, `%s` and the unit. DateTime values are shown as `HH:mm` or `d MMM HH:mm`. In practice the SSE `displayState` already carries openHAB's formatted value and is used as is. Exception: setpoint tiles format the raw state with the precision of their step, because openHAB's default pattern for `Number:Temperature` is `%.0f` (21.5 °C would show as "22 °C").
- **REQ-TILE-2:** read-only items are never given a control. This covers state descriptions with `readOnly: true`.
- **REQ-TILE-3:** the `wallpanel` metadata config can override `label`, `icon` (from a fixed built-in icon set), `order`, `step`, `min` and `max`. Icon names: `light`, `power`, `home`, `bell`, `alarm`, `warning`, `play`, `settings`, `water`, `wifi`, `eye`, `location`, `battery`, `camera`, `audio`, `refresh`, `up`, `down` (LVGL's built-in symbols; a bulb or thermometer would need a custom icon font). The built-in fonts cover ASCII plus `°`, so labels with other accented letters show gaps.
- **REQ-TILE-4:** "All off" button. A screen with two or more switchable light tiles shows an "All off" button. Switchable light tiles are Switch, Dimmer or Color items with the semantic property `Light` or the tag `Lighting`. The button sends OFF to each of those items.
- **REQ-TILE-5:** a tile shows a "pending" state from the touch until the new state arrives over SSE. If no state arrives within 3 s, the tile goes back to the old state and flashes an error.

### 3.2 Status line

- **REQ-STAT-1:** a top bar of 40 px shows:
  - the clock (SNTP, time zone Europe/Paris);
  - the Wi-Fi and openHAB connection state;
  - up to four status items.
- **REQ-STAT-2:** the status items are the items with the configured status tag (default `wp_status`). Examples: outdoor temperature, solar power, grid power, the camera alert type.
- **REQ-STAT-3:** a status item whose `wallpanel` metadata has `alert=true` is shown in red while it is ON or OPEN. Example: a camera alarm.

## 4. openHAB connection

- **REQ-OH-1:** settings: base URL (for example `http://192.168.1.139:8080`) and API token. The token is created in the openHAB user profile and stored in NVS. It is never shown again after it has been saved.
- **REQ-OH-2:** initial states come from the item list call in REQ-SCR-2.
- **REQ-OH-3:** live updates use the SSE state subscription:
  1. `GET /rest/events/states` returns a connection id;
  2. `POST /rest/events/states/<id>` sends the list of item names shown on any screen;
  3. the panel then receives `{item: {state, displayState, type}}` updates.

  A second stream, `/rest/events?topics=openhab/items/*/added,openhab/items/*/updated,openhab/items/*/removed` plus the same three topics for `openhab/rules/*`, triggers REQ-SCR-5. openHAB sends these as `event: message`; the state stream sends unnamed events, an `event: ready` with the connection id, and `event: alive` every 10 s (used as the stream watchdog: no data for 15 s means reconnect).
- **REQ-OH-4:** commands are sent with `POST /rest/items/<name>` as text/plain.
- **REQ-OH-5:** if a stream drops, the panel reconnects with backoff: 1, 2, 5, then 10 s. While disconnected, the status bar shows "openHAB offline" and tiles are greyed out.

## 5. Display behaviour

- **REQ-DISP-1:** after 120 s without a touch, the screen goes dark (backlight off). Any touch wakes it. The waking touch does not trigger a tile (an invisible layer catches it until the finger lifts). `dark_s` = 0 keeps the screen on. The setup screen (access point with QR code) never goes dark.
- **REQ-DISP-2:** optional night mode: between 23:00 and 06:00 the screen stays dark until touched. The screen goes dark when the night window starts; after a touch at night it goes dark again after `dark_s`, or after 30 s if `dark_s` is 0.

## 6. Setup and maintenance (reused from sensor-actor)

- **REQ-NET-1:** Wi-Fi setup reuses the sensor-actor approach:
  - wifi_mgr reconnect with AP fallback;
  - a captive portal on AP `WallPanel-XXXX`;
  - a small web UI with Home, Settings and Status pages.

  Settings: Wi-Fi, openHAB URL and token, screen list, status tag, timeouts.
- **REQ-NET-2:** a first-boot screen on the LCD shows the AP name and a QR code for joining it.
- **REQ-OTA-1:** OTA by uploading an image in the web UI (`POST /api/ota`), with rollback on a failed self-check.
- **REQ-LOG-1:** persistent error log and crash summary, as in sensor-actor. The panic handler runs from IRAM (`CONFIG_ESP_PANIC_HANDLER_IRAM`): from flash it hung on cache errors, so crashes became silent watchdog resets without a report.
- **REQ-NET-3:** the panel scans all channels and joins the strongest access point with the configured name (mesh networks have several).

## 7. Build and repository

- ESP-IDF v6.1 (`/workspace/esp`), target esp32s3, LVGL 9 via esp_lvgl_port.
- Repo: /workspace/oh-wallpanel, on GitHub as `ulijoergens/oh-wallpanel`. CI builds in GitHub Actions with `espressif/idf:v6.1`.
- Partitions (`partitions.csv`): 8 MB flash, with two OTA slots of 3 MB each, NVS (24 KB) and a 64 KB coredump partition.

## 8. Milestones

1. **Bring-up:** done 2026-11-21 (colour bars, touch, backlight verified on the board).
   - LCD, touch and backlight work;
   - an LVGL demo screen shows;
   - touch coordinates are correct;
   - no drift.
2. **Wi-Fi and settings:** done 2026-11-22 (verified on the board). Wi-Fi, captive portal and web UI settings (ported from sensor-actor), plus SNTP.
3. **openHAB client:** done 2026-11-22 (tested on the board through the console; unit tests pass).
   - the item list per tag;
   - SSE state subscription;
   - commands.
4. **Tiles and screens:** done 2026-11-22 (verified by touch on board 2; see `main/ui_tiles.c`).
   - tile types;
   - tab bar and status line;
   - "All off";
   - scene buttons.
5. **Polish:** backlight timeout, OTA, error log, CI. OTA update, rollback after a crash before confirmation, and the error log tested on the board 2026-11-22; CI in `.github/workflows/build.yml`.
6. **openHAB side:** tag the first items (`wp_home`, `wp_status`) and create the API token.
