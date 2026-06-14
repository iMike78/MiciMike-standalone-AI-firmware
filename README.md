# MiciMike AI Firmware

Alternative ESP-IDF firmware for the MiciMike Gen2 Nest Mini drop-in PCB
(ESP32-S3 + XMOS XU316). It turns the board into a standalone, privacy-conscious
AI voice device using the OpenAI Realtime API, an OpenAI-compatible service, or
a self-hosted Realtime LLM endpoint.

The original MiciMike PCB was born from a privacy problem: many people like the
Home/Nest Mini form factor, microphone placement, speaker, LEDs, and simple room
presence, but do not want an always-connected Google device in their home. The
Home Assistant firmware path solves that beautifully for people who already run
HA. This firmware is the other path: a way to use the same dedicated hardware
without requiring a Home Assistant installation at all.

The open voice hardware landscape is not empty: Home Assistant Voice Preview
Edition, ReSpeaker Lite, ESP32-S3-BOX, Atom Echo, and other ESPHome-based
devices all exist. What is much rarer is the software shape this project is
aiming for: dedicated AI conversation firmware that does not need Home
Assistant, Alexa, Google, or another smart-home controller underneath it.
MiciMike's niche is a drop-in replacement board for the Home/Nest Mini shell that can
operate as a self-contained AI voice endpoint, with local wake word detection,
hardware mute, web configuration, internet radio, LEDs, and direct cloud
or self-hosted Realtime API integration.

## Architecture

```text
Microphones
    |
    v
+------------------------------+
| XMOS XU316                   |
| AEC, noise reduction, AGC,    |
| voice/audio conditioning      |
+------------------------------+
    | I2S input to ESP32-S3
    v
+------------------------------+        WebSocket audio/events
| ESP32-S3                     | <----------------------------+
| WiFi, web UI, wake word,     |                              |
| media, device control,       | ----------------------------> |
| Realtime API client          |                              |
+------------------------------+                              |
    | I2S output                                                |
    v                                                           |
+------------------------------+        +----------------------+
| TLV320AIC3204 DAC / codec    |        | OpenAI Realtime API  |
| volume, EQ, line/headphone   |        | or compatible API    |
| output path                  |        +----------------------+
+------------------------------+
    |
    v
Power amplifier / speaker path
```

Notes:

- The XMOS stage handles audio cleanup such as echo cancellation, noise
  reduction, and gain control. This is a two-microphone design, not a beamforming
  array.
- The AIC3204 is the DAC/codec audio path. The speaker itself is driven
  through the board's amplifier/speaker path, not directly by the AIC3204.

## Features

- Standalone OpenAI Realtime voice assistant, no Home Assistant required.
- Configurable Realtime API endpoint for OpenAI-compatible services.
- Local microWakeWord wake word detection with embedded TFLite models:
  `Okay Nabu`, `Hey Jarvis`, `Hey Mycroft`.
- Wake word sensitivity control from the web UI.
- Server-side Realtime VAD with idle/no-speech timeout protection.
- Barge-in support while the assistant is speaking.
- Device-control tool calls for local actions such as radio playback and volume.
- Built-in web lookup tool for current facts, dates, holidays, news, prices, and
  other time-sensitive questions.
- Web UI for API keys, OpenAI Admin usage lookup, voice, conversation style,
  system prompt, wake word, sensitivity, volume, EQ, radio, language, and system
  status.
- UI localization for English, German, Spanish, Portuguese, Italian, Polish, and
  Hungarian.
- Internet radio playback for direct HTTP streams.
- Hardware mute switch with LED feedback.
- LED state animations for idle, listening, speaking, volume, zero-volume, and
  mute states.
- Token usage display from Realtime response usage and optional OpenAI Admin
  usage API.
- Captive portal provisioning on first boot / factory reset.
- Touch/button control path for volume and manual interaction; MPR121 hardware
  validation is still pending on final boards.

## Current Status

This firmware is usable, but still experimental. The main voice, radio, wake
word, web UI, settings, and LED flows are implemented. The biggest remaining
engineering work is audio polish and robustness across more networks, speakers,
and Realtime API behavior.

| Area | Status |
|------|--------|
| ESP32-S3 WiFi + NVS config | Implemented |
| Captive portal provisioning | Implemented |
| Web settings UI | Implemented |
| OpenAI Realtime WebSocket client | Implemented |
| Response token usage logging | Implemented |
| OpenAI Admin usage lookup | Implemented |
| microWakeWord models | Implemented |
| Wake word sensitivity | Implemented |
| AIC3204 volume / mute / EQ | Implemented |
| Internet radio | Implemented |
| Device-control tool calls | Implemented |
| Web lookup tool | Implemented |
| LED feedback | Implemented |
| Touch/button path | Implemented, needs final-board validation |
| Snapcast/Sendspin | Skeleton only |
| OTA updates | Not implemented |

## Build

Requires ESP-IDF v6.0.1 or a compatible ESP-IDF 6.x environment with ESP32-S3
support.

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Configure
idf.py set-target esp32s3

# Build
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash monitor
```

On Windows / ESP-IDF PowerShell:

```powershell
cmake --build build --parallel 1
$env:ESPPORT='COMX'
cmake --build build --target flash
```

## First Boot Configuration

On first boot, or after factory reset:

1. Connect to the WiFi network `MiciMike-Setup`.
2. Open the captive portal page if it does not open automatically.
3. Enter WiFi credentials, API key, wake word, and optional API endpoint.
4. The device saves settings and reboots into normal mode.
5. Open the web UI on the device IP for full configuration.

The normal web UI includes:

- API key and compatible Realtime endpoint.
- Optional OpenAI Admin key for organization usage stats.
- Wake word and sensitivity.
- Realtime voice and conversation style.
- Built-in and custom system prompt.
- UI language.
- Volume and EQ.
- Internet radio stations.
- Device name, heap status, RSSI, and restart/factory reset controls.

## Voice Behavior

The assistant automatically follows the user's spoken language. The UI language
setting only changes the web interface; it does not force the assistant's spoken
language. The assistant can also be asked to translate between languages in real
time.

The built-in system prompt tells the model to:

- follow the user's spoken language automatically,
- use `device_control` only for explicit local playback/volume commands,
- call `web_lookup` for facts that may have changed,
- never start radio playback unless the latest user request explicitly asks for
  it,
- briefly confirm successful device actions in the user's language.

Custom system prompt text from the web UI is appended after the built-in device
rules.

## Touch / Button Controls

The firmware contains the control path for:

| Action | Function |
|--------|----------|
| Volume up | Increase volume by 5% |
| Volume down | Decrease volume by 5% |
| Manual wake | Start a voice session |
| Factory reset | Return to setup mode |
| Cancel/session stop | End the current session |

Exact button/touch hardware behavior depends on the final board revision and
needs validation on hardware.

## Roadmap

- Improve Realtime response audio smoothness and buffering.
- Add OTA update support.
- Finish Snapcast/Sendspin playback.
- Add safer model hot-swap for wake word changes without reboot.
- Improve web UI structure by moving the embedded HTML into a separate source
  asset.
- Add more diagnostics for audio underruns, heap fragmentation, and API tool
  timing.
- Optional local conversation memory.

## License

Original MiciMike firmware code is licensed under the MIT License; see
`LICENSE`.

This repository also contains or fetches third-party code and model data under
their own licenses, including Apache-2.0 TensorFlow/microWakeWord assets and
Espressif components. See `THIRD_PARTY_NOTICES.md` and `licenses/` before
redistributing source or firmware binaries.

## Hardware

This firmware is designed for the
[MiciMike Gen2 Nest Mini drop-in PCB](https://github.com/iMike78/nest-mini-drop-in-pcb).

Visit [micimike.com](https://micimike.com) or the
[Crowd Supply campaign](https://crowdsupply.com/micimike-rev-devices) for more
information.
