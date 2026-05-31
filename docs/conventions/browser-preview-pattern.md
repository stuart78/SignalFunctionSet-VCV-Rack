# Browser preview pattern for display widgets

## Why

The VCV Library generates module thumbnails by running `Rack --screenshot 1.0`, which instantiates each module's widget **with no underlying Module** — `ModuleWidget->module` is `NULL`. Any display widget that early-returns on `!module` produces an empty dark slab in the browser, telling new users nothing about what the module does. Compare a Beat thumbnail showing a populated step grid vs. one showing a blank rectangle: the difference is whether the screenshot tells the module's story or hides it.

## The pattern

Every display widget that draws content based on module state must implement a `drawPreview(args)` method that paints a representative scene using hardcoded values, then route the `!module` case through it.

```cpp
struct MyDisplay : Widget {            // or OpaqueWidget
    MyModule* module = nullptr;
    std::shared_ptr<Font> font;

    void drawLayer(const DrawArgs& args, int layer) override;
    void drawPreview(const DrawArgs& args);   // module==NULL fallback
};

void MyDisplay::drawLayer(const DrawArgs& args, int layer) {
    if (layer != 1) {                   // not the emissive layer — pass through
        Widget::drawLayer(args, layer);
        return;
    }
    if (!module) {                      // browser screenshot
        drawPreview(args);
        return;
    }
    // ... existing live render path that reads from module->...
}

void MyDisplay::drawPreview(const DrawArgs& args) {
    // Paint a representative scene with hardcoded values.
    // No access to module->anything.
    // Use the same palette and layout helpers as drawLayer so the preview
    // looks like a real screenshot of the live module.
}
```

## What to put in the preview

The screenshot is the user's first impression. Show what the module *does*, not just that it has a display. Concrete examples from this codebase:

| Module | Preview content |
|---|---|
| Beat | Step grid with kick on 0/4/8/12, ghost hits, accent rings, STEPS tab selected, pattern 1 selected, repeats=4 |
| Note | Pitch matrix with an ascending melody in Major scale, STEPS mode, pattern 1 active |
| Wave | Live 2-peak wave preview + 3 filled snapshot cells (sine, 2-peak, triangle) with playhead pointer |
| Phase | Two synthesized waveform shapes (drum + pad) with loop handles + playheads |
| Meter | "120.0 BPM", "4/4", "BAR 1", populated subdivision tick rows, position tracker on beat 1 |
| Swell | Three stacked rises decaying over time on the scope |
| Overtone | Fundamental + harmonics 2–4 with composite waveform |
| Intone | "Ah" vowel formant spectrum (F1=730, F2=1090, F3=2440…) |
| Muse | Slider/tap table with 8 representative positions, lit LEDs across bits, "E4" status |
| Swing | Pendulum mid-swing pose, two sector arcs lit, fading tip trail |

## Mechanical rules

1. **Use the same palette constants and layout helpers as `drawLayer`.** This is what makes the preview look like a real screenshot. `computeLayout()` is module-independent — call it.
2. **Hardcode all state.** No globals, no static `Module*`. Just constant arrays / scalars right inside the function. Easier to tweak when the screenshot needs adjusting.
3. **Pick a scene that shows the module's character.** A blank or near-empty preview is worse than no preview. If the module's character is "sequenced patterns," show a pattern. If it's "evolving spectra," show a spectrum mid-evolution.
4. **Lazy font load works fine in preview** — the same pattern as `drawLayer`.

## Testing

```bash
./build.sh dev
# Or for stable testing, point at the prod plugin:
~/path/to/Rack --screenshot 1.0
ls ~/Library/Application\ Support/Rack2/screenshots/SignalFunctionSet-dev/
open ~/Library/Application\ Support/Rack2/screenshots/SignalFunctionSet-dev/Beat.png
```

The screenshot tool writes one PNG per registered module. The PNG should show the populated preview, not a dark slab.

## When this applies

**Any new module with a custom `drawLayer` widget gets a `drawPreview` from day one.** Treat it as a required part of the display widget contract, alongside lazy font loading and the `layer != 1` check.

Modules without displays (just panel knobs/jacks, no NanoVG widget) don't need this — Rack's screenshot of the panel SVG is sufficient.
