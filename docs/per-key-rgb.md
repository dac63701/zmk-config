# Per-key RGB map

The configuration uses the pinned `darknao/zmk-perkey_ug` revision in
`config/west.yml`. It drives the existing 61-pixel WS2812 chain and preserves
the `EXT_POWER` LED rail control.

## Brightness and power policy

- Startup brightness: 15%
- Hard maximum: 20%
- Brightness step: 5%
- Idle on battery: RGB switches off after 30 seconds.
- Idle on USB: RGB remains active for five minutes before switching off.
- USB: RGB remains available.

RGB settings changed with the Fn-layer controls are persisted by ZMK and can
override the compiled startup brightness. Clear settings or use Fn+Y/Fn+U to
return to the intended range during testing.

The renderer keeps four states separate: the persisted user on/off choice,
runtime LED-rail power, the temporary Fn overlay, and idle suspension. Entering
Fn or idle no longer overwrites the saved user choice. A USB connection is
treated as activity and reasserts the rail only when RGB is meant to be on.

The battery monitor may disable the rail below its unplugged warning threshold,
but it never enables the rail. This prevents it from fighting RGB toggle and
idle decisions; on USB it clears the battery veto and lets the RGB driver own
the handoff.

## Fn layer

Holding the Fn (`MO(1)`) key automatically pauses the current animation and
shows the color legend. Releasing Fn resumes the same animation:

- Cyan: Bluetooth profile controls
- Blue: navigation
- Green: RGB controls
- Magenta: volume/mute
- Purple: media playback
- Orange: function/system keys
- Red: clear Bluetooth profiles
- White: the Fn key

Additional RGB controls are placed on previously transparent Fn bindings:

| Keys | Action |
| --- | --- |
| Fn+T / Fn+Y | brightness up / down |
| Fn+U / Fn+I | animation speed up / down |
| Fn+O / Fn+] | saturation up / down |
| Fn+P / Fn+[ | hue up / down |
| Fn+Z / Fn+X | previous / next effect |
| Fn+Space | RGB toggle |
| Fn+Right Shift | unlock ZMK Studio |

## Required physical verification

`pixel-lookup` is row-major: LED 0 is Esc and the lookup proceeds left-to-right
through each row to LED 60 at Right Ctrl. This matches the observed Fn-legend
positions; the horizontal effect handles visual direction in the renderer.

Before treating the map as final, use a diagnostic build to identify LED 0,
1, and 2 and update `pixel-lookup` if the chain snakes across rows. For each
LED index, record the physical key, matrix position, and approximate X/Y
coordinates. Test the whole Fn layer afterward: each lit key must match its
Fn action and releasing Fn must immediately restore the normal effect.

## Current fork behavior

`CONFIG_EXPERIMENTAL_RGB_LAYER=y` is required for the pinned fork to process
the `underglow_layer` node. Build-time patches add single-piece keyboard layer
events and make the map an automatic overlay. The fifth effect is a horizontal
rainbow: every physical row travels left-to-right instead of following the
serpentine LED wiring. The startup speed is 1. The animation updates every
20 ms (50 Hz), uses tenth-degree phase steps, and positions each LED at the
physical center of its key across the 15u board. Wide keys therefore create
proportionally larger spatial gaps instead of being treated as 1u keys.

## ZMK Studio

The build enables ZMK Studio's USB RPC endpoint. Connect the flashed keyboard
over USB, hold Fn and press Right Shift to unlock it, then open ZMK Studio.
Studio can edit the runtime keymap; the RGB lookup table, color legend, and
custom RGB effects remain firmware configuration and require a rebuild.
