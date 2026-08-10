# Open Media Player v1.2 — ESP32-S3-LCD-1.3 Prism

This is the ESP32-S3-LCD-1.3 Prism hardware profile of the stable OMP v1.2 player.

## Project Description
Open Media Player V1.2 Video Player turns the ESP32-S3-LCD-1.3 Prism board into a standalone MJPEG media player. The firmware scans the SD card for `.mjpeg` files in `/mjpeg`, decodes them frame-by-frame, and renders video on the LCD in a continuous loop. A hardware button lets you skip to the next clip during playback.


## Acknowledgement

Open Media Player is gratefully inspired by the original ESP32-C6-LCD-1.47
video-player project by The Last Outpost Workshop. Thank you for the original
project, tutorial, and inspiration behind this player.

- Support the original author: [Buy The Last Outpost Workshop a coffee](https://www.buymeacoffee.com/thelastoutpostworkshop)
- Prepare videos: [Open Video Conversion Studio](https://thelastoutpostworkshop.github.io/video_conversion/)

## Arduino IDE settings

- Board: `ESP32S3 Dev Module`
- USB CDC On Boot: `Disabled`
- PSRAM: `Disabled`
- Flash size: `4 MB`

Keep the established libraries unchanged:

- ESP32 Arduino core 3.2.0
- Arduino GFX Library 1.5.9
- JPEGDEC 1.8.4
- Dev Device Pins 0.0.3 (not used by this S3 sketch)

Serial diagnostics are explicitly sent to UART0: TX GPIO43 and RX GPIO44,
which are connected to the board's CH343 USB-to-UART bridge. Use 115200 baud.

## Install

Copy the sketch and `MjpegClass.h` into a new Arduino sketch folder named
`OMP_ESP32_S3_LCD_1_3_Prism`, then open `OMP_ESP32_S3_LCD_1_3_Prism.ino`.

On the SD card, copy `config.json`, `gpio.json`, and optionally `playlist.txt`
to the card root. Put video files in `/mjpeg`.

As most of the 'Prism' supplied boards do not have hardware Boot or Reset buttons,connect a normally-open momentary switch between the GPIO1 header pin and GND for OMP controls. A short press skips to the next video; hold for at least
750 ms to pause/resume. GPIO0 is reserved for bootloader entry and must not be
used as the normal OMP playback button.

## Display mode

Set `display_mode` in `config.json`:

- `"fit"` preserves the entire JPEG frame, leaving unused areas black.
- `"fill"` scales the frame to cover the 240 x 240 panel and crops equally from
  opposite edges where necessary. It needs a checked 113 KB working buffer;
  if that cannot be allocated, OMP logs a warning and uses `fit` instead.

`fill` is the supplied default. It performs more display work than `fit`, so
the actual frame rate may be lower than `target_fps` for large source frames.

## Image rotation

Set `image_rotation` in `config.json` to `0`, `90`, `180`, or `270` for a
clockwise rotation applied to every video by default. A playlist line can
override it for one file:

```text
portrait-video.mjpeg | 90
landscape-video.mjpeg | 0
```

The spaces around `|` are optional. Rotation uses the same checked working
buffer as `fill`; it supports source frames up to 57,600 pixels (including the
normal 240 x 240 format). If it is unavailable or a frame is larger, OMP
reports the condition and plays the original unrotated frame safely.

## Board profile

The profile uses the display and SD-MMC wiring from Waveshare's supplied
examples: ST7789 at 240x240, display SPI GPIO 38/39/40/41/42, backlight GPIO
20, and 1-bit SD-MMC GPIO 21/18/16. GPIO 17 is held high as the card CS line.
The validated fitted-panel configuration is rotation 1, zero row offset, and
an 8 MHz display SPI rate. No SD-card auto-format is performed.
