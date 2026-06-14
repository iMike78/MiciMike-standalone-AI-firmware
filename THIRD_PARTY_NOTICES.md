# Third-party notices

This repository's original MiciMike firmware code is licensed under the MIT
License in `LICENSE`.

Some files and build-time dependencies are not original MiciMike code and remain
under their own licenses. Do not represent every file in this repository, every
fetched ESP-IDF component, or every generated firmware binary as purely MIT
licensed.

## Vendored source files

### TensorFlow Lite Micro microfrontend

Path: `main/microfrontend/`

Most files in this directory are from TensorFlow and are licensed under the
Apache License 2.0. Their original copyright and license headers are preserved
in the files.

### KissFFT

Paths:

- `main/microfrontend/kiss_fft.c`
- `main/microfrontend/kiss_fft.h`
- `main/microfrontend/_kiss_fft_guts.h`

These files are licensed under the BSD-style KissFFT license by Mark Borgerding.
The original copyright and license headers are preserved in the files.

## Embedded wake word models

Paths:

- `main/okay_nabu_model.h`
- `main/hey_jarvis_model.h`
- `main/hey_mycroft_model.h`
- `main/alexa_model.h`

These headers contain TFLite model data converted from the ESPHome
`micro-wake-word-models` repository:

https://github.com/esphome/micro-wake-word-models

The model repository is licensed under Apache License 2.0. The model manifests
attribute the models to Kevin Ahrendt.

## ESP-IDF managed components

The ESP-IDF component manager fetches dependencies listed in `main/idf_component.yml`
and locked in `dependencies.lock`. Their licenses apply separately.

Direct dependencies currently include:

- `espressif/cjson` - MIT
- `espressif/esp-tflite-micro` - Apache License 2.0, with bundled third-party code under their own licenses
- `espressif/esp-nn` - Apache License 2.0
- `espressif/esp_websocket_client` - Apache License 2.0
- `espressif/led_strip` - Apache License 2.0
- `espressif/esp_audio_codec` - Espressif Modified MIT License

Important: `espressif/esp_audio_codec` is not a plain MIT dependency. Its
license permits use exclusively with Espressif Systems products and prohibits
redistribution for use with non-Espressif products. This is compatible with this
ESP32-S3 firmware target, but it should be disclosed clearly when publishing
source or binaries.

## License text copies

Copies of common third-party license texts used by this project are kept under
`licenses/`.
