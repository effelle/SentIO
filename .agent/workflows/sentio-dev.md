# Sentio Development Workflow & Knowledge Base

**Goal:** Create a "Universal" Input Subsystem for ESPHome that fixes the fragmentation and poor user experience of cheap embedded displays (CYD, Sunton, etc.).

## Core Philosophy: The Proxy Pattern
To avoid writing 50 different hardware drivers, use a **Man-in-the-Middle (Proxy)** architecture.
- **Source:** Existing ESPHome touchscreen platform (internal).
- **Component (`sentio`):** Reads source, sanitizes, applies logic, publishes clean events.
- **Consumer:** LVGL, HA, Lambda listen to `sentio`.

## Problem Solutions
1.  **"Wake-up Click"**: Suppression Logic (swallow first click if sleeping).
2.  **Sleep of Death**: Safety Check (reset_pin check) -> "Soft Sleep" if unsafe.
3.  **Ghost Touches**: Min-Frame Filter (ignore < N ms).
4.  **Coordinate Hell**: Pipeline Calibration (Swap -> Invert -> Offset).
5.  **Swipe is a Click**: Gesture Debounce (State Machine: Tap vs Drag).

## Implementation Phases
1.  **Passthrough POC**: Forward touches, verify with "Red Dot".
2.  **Sleep Doctor**: Timeout, Wake-up logic, Safety check.
3.  **Calibrator**: `swap_xy`, `invert_x`, `invert_y`.
4.  **Gesture Engine**: State Machine (IDLE -> START -> DRAG), Swipe vs Tap.

## Pro-Tips
- **I2C Scanning**: Toggle RST/INT manually before Wire.begin().
- **Visual Debugging**: Use "Red Dot" (filled_circle at touch coords).
- **Poll Order**: Sentio config should be below hardware driver.
- **Multi-Touch**: Stick to single-point for MVP.
