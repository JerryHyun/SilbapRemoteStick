# Third-party notices

SilbapRemoteStick includes or adapts the following works. Copy each project's
full license text into `licenses/` when its code or data is first added.

| Component | Use | License |
|---|---|---|
| [MainasuK/RemoteUSB](https://github.com/MainasuK/RemoteUSB) | base firmware: web UI, WebDAV, OTA, LCD, LED; the WebDAV server at /dav (main/webdav.c, main/webdav_xml.c) was adapted from its main/webdav.c and rewritten for this firmware; main/esp_lcd_st7735.c/.h are LilyGO's T-Dongle-S3 factory ST7735 panel driver, vendored unchanged from its main/ ([licenses/RemoteUSB-MIT.txt](licenses/RemoteUSB-MIT.txt)) | MIT |
| [robbellie/embroidery-stick](https://github.com/robbellie/embroidery-stick) | ideas and adapted code: virtual FAT, captive portal, Web Tools layout | MIT |
| [ESP-IDF](https://github.com/espressif/esp-idf), esp_tinyusb, mdns | framework and components | Apache-2.0 |
| [TinyUSB](https://github.com/hathach/tinyusb) | USB stack | MIT |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) | browser flasher page | Apache-2.0 |
| [qrcodegen](https://github.com/nayuki/QR-Code-generator) | QR codes on the LCD; main/qrcodegen.c/.h are Project Nayuki's single-file C encoder, vendored unmodified ([licenses/qrcodegen-MIT.txt](licenses/qrcodegen-MIT.txt)) | MIT |
| [Unicode Unihan database](https://www.unicode.org/charts/unihan.html) | Han readings (kMandarin, kHangul, kJapanese) | Unicode License v3 |
| [Ark Pixel font](https://github.com/TakWolf/ark-pixel-font) | LCD font | SIL OFL 1.1 |
| [GNU Unifont](https://unifoundry.com/unifont/) | LCD font glyph bitmaps, compiled from unifont-16.0.04.hex into main/font_data.c by tools/gen_font.py (OFL option) ([licenses/Unifont-OFL.txt](licenses/Unifont-OFL.txt)) | SIL OFL 1.1 (dual-licensed) |

The license terms of the two fonts and the Unifont dual license are checked
again before the font data is committed.
