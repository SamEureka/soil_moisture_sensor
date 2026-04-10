# Soil Moisture Sensor Gemini

This project reads soil moisture and light levels from a `seeed_xiao_esp32c3` board, publishes readings to Adafruit IO via MQTT, and exposes a small local web UI.

## Secrets workflow

- `secrets.ini.template` is tracked in Git and documents the required build-time configuration.
- Copy `secrets.ini.template` to `secrets.ini` and update the placeholder values with your own Wi-Fi and Adafruit IO credentials.
- `secrets.ini` is intentionally ignored by Git via `.gitignore` so local secrets are not committed.
- `platformio.ini` includes `secrets.ini` through `extra_configs`, so the actual credentials are applied at build time.

## Setup

1. Copy the template:

   ```sh
   cp secrets.ini.template secrets.ini
   ```

2. Edit `secrets.ini` with your real values.

3. Build and upload using PlatformIO.

## Notes for contributors

- When adding new configurable flags, update both `secrets.ini.template` and any local `secrets.ini` copies.
- Keep `secrets.ini` out of source control to avoid leaking credentials and local device configuration.
