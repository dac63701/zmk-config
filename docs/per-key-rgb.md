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

When the layer-map RGB effect is selected, holding the Fn (`MO(1)`) key shows
the color legend:

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

`pixel-lookup` is currently the explicit row-major assumption: LED 0 is Esc,
then the keymap proceeds left-to-right through each row to LED 60 (Right Ctrl).
The electrical chain is not documented in this repository, so this is the only
part that cannot be proved from source files.

Before treating the map as final, use a diagnostic build to identify LED 0,
1, and 2 and update `pixel-lookup` if the chain snakes across rows. For each
LED index, record the physical key, matrix position, and approximate X/Y
coordinates. Test the whole Fn layer afterward: each lit key must match its
Fn action and releasing Fn must immediately restore the normal effect.

## Current fork behavior

`CONFIG_EXPERIMENTAL_RGB_LAYER=y` is required for the pinned fork to process
the `underglow_layer` node. Its built-in per-layer map is an RGB *effect*:
cycle effects with Fn+X until the map effect is selected, then hold Fn to show
the legend. The fork does not yet draw the legend as an overlay over a normal
rainbow or restore an animation on release. That final behavior requires a
small patch to the fork's RGB renderer.

## ZMK Studio

The build enables ZMK Studio's USB RPC endpoint. Connect the flashed keyboard
over USB, hold Fn and press Right Shift to unlock it, then open ZMK Studio.
Studio can edit the runtime keymap; the RGB lookup table, color legend, and
custom RGB effects remain firmware configuration and require a rebuild.
