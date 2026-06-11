# SentIO - AGENTS.md

## Project Overview
SentIO is an ESPHome custom component providing a dynamic LVGL overlay system with touch gestures, sleep/wake management, SD card support, and Home Assistant API services. Targets ESP32 ESP-IDF. 

## Build / Test Commands

YOu cannot build the firmware. This task is up to the user.
The yaml files used for the tests are:

- real_config_with_sd.yaml
- real_config.yaml

### Single Test / Validation
```bash
# Validate YAML config without building
esphome config real_config_with_sd.yaml

# Clean build (removes .esphome/build)
esphome clean real_config_with_sd.yaml
```

### Linting / Type Checking
```bash
# Python lint (component __init__.py)
ruff check components/sentio/__init__.py
ruff format components/sentio/__init__.py

# C++ static analysis (if clang-tidy available)
clang-tidy components/sentio/*.cpp -- -I./components/sentio -std=gnu++17
```

## Code Style Guidelines

### Python (components/sentio/__init__.py)
- **Imports**: Group stdlib, third-party, ESPHome internal. Use absolute imports from `esphome.*`
- **Type hints**: Use for function signatures; `cv.Schema` dict types inferred
- **Constants**: `UPPER_SNAKE_CASE` for config keys (`CONF_*`), module-level
- **Async**: All `to_code()` and automation handlers are `async def`
- **Validation**: Use `cv.Schema` with custom validators (`_sd_card_schema`) for cross-field rules
- **Error handling**: Raise `cv.Invalid` with descriptive messages for config errors

### C++ (components/sentio/*.cpp, *.h)
- **Standard**: C++17 (`gnu++17`), ESP-IDF framework
- **Includes**: ESPHome headers first, then LVGL, then stdlib. Use `#pragma once`
- **Namespaces**: `namespace esphome { namespace sentio { ... } }`
- **Naming**: 
  - Classes: `PascalCase` (`SentioComponent`, `SdCardManager`)
  - Methods: `snake_case` (`set_touch_source`, `handle_widget_event`)
  - Members: `snake_case_` trailing underscore (`touch_source_`, `sleep_timeout_ms_`)
  - Constants: `kPascalCase` or `UPPER_SNAKE_CASE`
  - Enums: `PascalCase` enum class (`TouchState`, `SdMode`)
- **Types**: Use fixed-width (`int8_t`, `uint32_t`, `float`); avoid `int`/`long`
- **Conditionals**: `#ifdef USE_SENTIO_SD` / `#ifdef USE_SENSOR` for optional features
- **Overrides**: Mark with `override` keyword
- **Callbacks**: Use `std::function<void()>` + `CallbackManager` for automation triggers
- **Static singleton**: `static SentioComponent* instance` for lambda access

### ESP-IDF Component Dependencies
Declare required IDF components in `__init__.py` via `CORE.data[KEY_ESP32][KEY_EXCLUDE_COMPONENTS].discard()`:
- `fatfs`, `sdmmc`, `vfs`, `driver`, `spi_flash` for SD card support
- Set SDK config options via `add_idf_sdkconfig_option()`

### Configuration Schema
- Define `CONF_*` constants at module top
- Use `cv.Optional` with sensible defaults
- Use `cv.use_id()` for component references
- Use `automation.validate_automation()` for triggers
- Extend `cv.COMPONENT_SCHEMA` for base component fields

### Git / Commits
- Commit messages: Conventional format (`feat:`, `fix:`, `refactor:`)
- Branch: `stage` for development, `main` for releases
- External components: GitHub source with `@stage` ref for bleeding edge

## Key Files
- `components/sentio/__init__.py` - Python schema & codegen
- `components/sentio/sentio.h` - C++ class declarations
- `components/sentio/sentio.cpp` - C++ implementation
- `components/sentio/sentio_sd.h/cpp` - SD card manager
- `real_config_with_sd.yaml` - Reference ESPHome config (ESP-IDF)
- `library.json` - PlatformIO library metadata

## ESPHome Version
Target: **2026.5.1+** (ESP-IDF 5.x). Do not use 2026.1.x patterns.