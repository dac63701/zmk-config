# Per-key RGB map

The configuration uses the pinned `darknao/zmk-perkey_ug` revision in
`config/west.yml`. It drives the existing 61-pixel WS2812 chain and preserves
the `EXT_POWER` LED rail control.

## Brightness and power policy

- Startup brightness: 15%
- Hard maximum: 20%
- Brightness step: 5%
- Idle: RGB is switched off and the configured external LED power integration
  is allowed to remove LED rail power.
- USB: RGB remains available.

RGB settings changed with the Fn-layer controls are persisted by ZMK and can
override the compiled startup brightness. Clear settings or use Fn+Y/Fn+U to
return to the intended range during testing.

## Fn layer

Holding Fn (Right GUI) replaces the active RGB effect with the color legend:

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

## Required physical verification

`pixel-lookup` is currently the explicit row-major assumption: LED 0 is Esc,
then the keymap proceeds left-to-right through each row to LED 60 (Right Ctrl).
The electrical chain is not documented in this repository, so this is the only
part that cannot be proved from source files.

Before treating the map as final, use a diagnostic build to identify LED 0,
1, and 2 and update `pixel-lookup` if the chain snakes across rows. For each
LED index, record the physical key, matrix position, and approximate X/Y
coordinates. Test the whole Fn layer afterward: each lit key must match its
Fn action and releasing Fn must immediately restore the normal effect.
