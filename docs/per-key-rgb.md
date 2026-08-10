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

Low-battery protection uses raw cell voltage rather than the modeled Windows
percentage:

- At or below 3.30 V for three consecutive five-second samples, the RGB rail
  is disabled.
- At or above 3.40 V, the low-battery veto clears.
- At or below 3.00 V for three consecutive samples, the nPM1300 enters ship
  mode.
- USB power bypasses both the RGB cutoff and ship-mode entry.
- Measurements outside the physically plausible 2.50-5.00 V range are ignored
  and can never switch off the RGB rail or enter ship mode.

Battery reporting uses Nordic's nRF Fuel Gauge algorithm on the nRF52840 with
the supplied LP602760 1000 mAh, 4.20 V rechargeable-cell model. The nPM1300
provides voltage and bidirectional current measurements, and ZMK reports the
model's state of charge directly rather than converting voltage linearly. This
is an interim model for the similarly sized MakerFocus 1100 mAh pack; its
coefficients are kept unchanged.

The pack and PCB have no cell thermistor, so the algorithm uses the model's
22 C profile. It updates every 2 seconds while active on battery, every 10
seconds while idle, every 500 ms on USB/while charging, and not during system
off/deep sleep. Raw-voltage protection remains on its independent five-second
schedule regardless of the fuel-gauge update rate. The algorithm runs in a
dedicated low-priority thread with its own stack, so it cannot block ZMK's
keyboard and RGB system work queue. ZMK battery reads only consume the latest
completed estimate and never execute the algorithm synchronously.

RGB settings changed with the Fn-layer controls are persisted by ZMK and can
override the compiled startup brightness. Clear settings or use Fn+Y/Fn+U to
return to the intended range during testing.

The renderer keeps four states separate: the persisted user on/off choice,
runtime LED-rail power, the temporary Fn overlay, and idle suspension. Entering
Fn or idle no longer overwrites the saved user choice. A USB connection is
treated as activity and reasserts the rail only when RGB is meant to be on.

RGB power transitions are serialized. Turning lighting off stops frame
generation, writes and latches an all-black frame, and only then disables the
LED rail. Turning it on enables the rail, waits 5 ms for it to stabilize,
latches a black reset frame, and then paints the requested animation or Fn
map. Animation work checks the runtime state both before rendering and before
transmission, so a queued stale frame cannot relight pixels after shutdown.

The battery monitor may disable the rail once when the unplugged warning
threshold is crossed, but it never repeatedly forces the rail off or enables
it. This prevents it from fighting RGB toggle and idle decisions; on USB it
clears the battery veto and lets the RGB driver own the handoff.

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

While Fn is held, Backspace is the charge indicator: amber means the nPM1300
is actively charging, green means charging completed, and blue means USB power
is present but the charger is not reporting either state.

Brightness and saturation changes repaint the active Fn RGB map immediately.
Hue, speed, and effect controls target the underlying animation, which becomes
visible when Fn is released; the Fn legend keeps its configured key colors.
The renderer rechecks the actual top keymap layer before animation frames and
during USB/activity wake transitions, so the Fn map remains authoritative while
Fn is held on either battery or USB power.

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
