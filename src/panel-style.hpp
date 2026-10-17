#pragma once
// =============================================================================
// Panel style — the design system, as code.
//
// Panels used to be drawn in Illustrator, which meant every label had to be
// outlined to paths (Rack ignores <text> in a panel SVG). Chance carries 84 text
// paths, Band 106; changing one word meant going back to Illustrator. So text is
// drawn at runtime instead, in Figtree, from this one place — and the SVG holds
// only geometry.
//
// What is actually VISIBLE on a finished panel is a shorter list than it looks,
// because Rack draws each component on top of its own footprint: anything in the
// SVG the size of a knob or a jack is hidden underneath it. The design is
// therefore the background, the plates, the screens, the labels, and any mark
// that sits OUTSIDE a component's footprint.
// =============================================================================

#include "plugin.hpp"
#include <string>
#include <vector>

namespace sfs {

// ── palette ─────────────────────────────────────────────────────────────────
static const NVGcolor PANEL      = nvgRGB(0xF0, 0xF0, 0xF0);  // the faceplate
static const NVGcolor INK        = nvgRGB(0x23, 0x1F, 0x20);  // label text on the faceplate
static const NVGcolor INK_SOFT   = nvgRGB(0x6A, 0x6A, 0x78);  // sub-labels, pot-position notes
static const NVGcolor PLATE      = nvgRGB(0x1A, 0x1A, 0x1A);  // dark inset grouping a section
static const NVGcolor PLATE_INK  = nvgRGB(0xFF, 0xFF, 0xFF);  // label text on a plate (white, as the designer draws it)
static const NVGcolor HAIRLINE   = nvgRGB(0xB2, 0xB2, 0xB2);  // reticules
static const NVGcolor RULE       = nvgRGB(0xA8, 0xA8, 0xB6);  // pot-to-jack connectors AND section dividers
static const float RULE_W        = 0.169f;                    // mm: both, as the designer strokes them

// display palette — what the screens clear to and draw with
static const NVGcolor SCREEN_BG   = nvgRGB(0x1A, 0x1A, 0x32);
static const NVGcolor SCREEN_BLUE = nvgRGB(0x00, 0x97, 0xDE);
static const NVGcolor SCREEN_DEEP = nvgRGB(0x0D, 0x59, 0x86);
static const NVGcolor SCREEN_LINE = nvgRGB(0x0D, 0x59, 0x88);
static const NVGcolor SCREEN_PURP = nvgRGB(0x35, 0x35, 0x4D);
static const NVGcolor SCREEN_PMID = nvgRGB(0x4A, 0x4A, 0x66);
static const NVGcolor SCREEN_HOT  = nvgRGB(0xEC, 0x65, 0x2E);
static const NVGcolor SCREEN_TEXT = nvgRGB(0xE8, 0xE8, 0xF0);
static const NVGcolor SCREEN_DIM  = nvgRGB(0x8A, 0x8A, 0xA5);

// ── grid ────────────────────────────────────────────────────────────────────
// One grid, one unit: 1HP, in BOTH axes. Controls sit on intersections, so a
// layout is stated in whole cells and scales with the panel instead of being a
// pile of loose millimetre values. A panel is 128.5mm tall = 25.29HP, so there
// are 25 usable rows and the remainder is left as a margin at the FOOT of the
// panel rather than distributed.
static const float HP = 5.08f;
static inline float hp(float n) { return n * HP; }
static const int GRID_ROWS = 25;

// Jacks are 8.03mm and trimpots 6.05mm, so neighbours need 2 cells between them;
// a pot and its own jack sit 2 cells apart on the same row and are joined by a
// hairline, which is what says they belong together.
static const float PAIR_SPAN = 2.f;      // cells from a pot to its jack

// ── type ────────────────────────────────────────────────────────────────────
// Measured off the designer's 2026-09 panels (canon, count, spool, field, flock,
// sigma, wheel, carillon...), so a code-drawn panel and a finished one match:
//   labels  Figtree Regular, cap height 1.66 mm = 2.37 mm type, no tracking,
//           their BASELINE a fixed gap above the control (LABEL_GAP_*)
//   title   Figtree ExtraBold, cap height 1.90 mm = 2.71 mm type, no tracking,
//           one HP in (half an HP on 8HP and under), baseline 8.55 mm, so its
//           cap centre sits level with the logo's centre at 7.62 mm
// These are EM sizes, as Figma and SVG mean a font size. nanovg does not: its
// size is the font's ascender-to-descender height (stb_truetype's
// ScaleForPixelHeight), which for Figtree is 950 + 250 = 1.2 em, so it is
// handed size * NVG_EM. Without that every label came out at 5/6 of the
// designer's (seen side by side with Canon).
// The code used to set labels at 3.3 (a 2.75 mm em) tracked 0.1 mm and the
// title at 5.6 SemiBold (4.67 mm em), against the designer's 2.37 and 2.71.
// NOTE is for the small annotations that mark what a pot position means.
static const float TYPE_LABEL = 2.37f;  // mm
static const float TYPE_NOTE  = 2.0f;   // mm — pot-position notes, ranges
static const float TYPE_TITLE = 2.71f;  // mm — the module name
static const float TITLE_BASELINE = 8.55f;  // mm from the top
static const float NVG_EM = 1.2f;           // Figtree: nanovg size per em (see above)

// ── screen type ─────────────────────────────────────────────────────────────
// One size for on-screen text, for the same reason there is one size for panel
// labels. Note and Beat draw every string at 9 units on a 174-unit display,
// which is exactly SCREEN below in mm — stating it here means a new display
// cannot drift away from them by accident, which is precisely what happened
// with Key and Loom (both invented sizes proportional to their own height, so
// each module's text came out a different size).
// SCREEN_SMALL is the screen's counterpart to TYPE_NOTE: dense rows only, where
// the full size genuinely will not fit.
// Neither carries letter spacing — Note and Beat set none, and tracking is what
// made the same nominal size read as a different face.
static const float TYPE_SCREEN       = 2.38f;  // mm
static const float TYPE_SCREEN_SMALL = 1.90f;  // mm

// SCREENS ARE SET IN SHARE TECH MONO, panels in Figtree. Two faces, two jobs:
// the faceplate is print and the screen is a readout, and a readout wants a
// mono face so a changing number does not shuffle the characters either side of
// it. Every other module in the plugin already loads this one — Note, Beat,
// Chance and 26 more — and TYPE_SCREEN was measured against it, so a display
// that reaches for panelFont() instead comes out in the wrong face at a size
// chosen for a different one. Key, Loom, Slide and OP MORPH all did.
//
// It comes from Rack's own res/, not ours: it ships with Rack, so bundling a
// second copy would only add a way for the two to disagree.
static inline std::shared_ptr<Font> screenFontFace() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

static inline void screenFont(NVGcontext* vg, const std::shared_ptr<Font>& f,
                              float mm = TYPE_SCREEN) {
	nvgFontFaceId(vg, f->handle);
	nvgFontSize(vg, mm2px(mm));
	nvgTextLetterSpacing(vg, 0.f);
}

// ── spacing ─────────────────────────────────────────────────────────────────
// Labels sit ABOVE their control: each gap is control centre to label BASELINE.
// The designer's newest panels (canon, count) put a trimpot's and a jack's label
// 5.3 mm above it; that leaves a jack 1.3 mm of air, the tightest anywhere, so
// jacks get 5.8 here and the larger knobs keep about 2 mm clear of their edge.
static const float LABEL_GAP_TRIM = 5.3f;
static const float LABEL_GAP_JACK = 5.8f;
static const float LABEL_GAP_KNOB = 6.8f;        // RoundBlackKnob, 9.6 mm
static const float LABEL_GAP_KNOB_LARGE = 8.2f;  // RoundLargeBlackKnob, 12.2 mm
static const float LABEL_GAP_ROGAN = 7.3f;       // Rogan1P, 10.6 mm
static const float LABEL_GAP_ROGAN_LARGE = 9.1f; // Rogan3P, 14.1 mm
static const float PLATE_PAD      = 2.6f;    // inset padding around a grouped section
static const float PLATE_RADIUS   = 1.35f;   // the designer's plates and the tools' (panel_reticules.py)

// Whether Rack's own window has focus. Hover events keep arriving while another
// application is in front, so an instrument that plays on hover plays whenever
// the pointer happens to cross it -- while you are reading a manual, or dragging
// a file past. Nobody expects a synth in the background to make a noise.
static inline bool windowFocused() {
	return APP && APP->window && APP->window->win
	    && glfwGetWindowAttrib(APP->window->win, GLFW_FOCUSED) != 0;
}

static inline std::shared_ptr<Font> panelFont() {
	return APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Figtree-Regular.ttf"));
}
static inline std::shared_ptr<Font> panelFontBold() {
	return APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Figtree-SemiBold.ttf"));
}
static inline std::shared_ptr<Font> panelFontTitle() {
	return APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Figtree-ExtraBold.ttf"));
}

// ── labels ──────────────────────────────────────────────────────────────────
// Positions are in mm and are the CONTROL's centre; the widget applies the gap,
// so moving a control means changing one number in the widget constructor and
// nothing else.
struct PanelLabels : Widget {
	enum Kind { LABEL, NOTE, TITLE, ON_PLATE };
	struct Item { Vec pos; std::string text; Kind kind; int align; };
	struct Link { Vec a, b; bool onPlate; };
	std::vector<Item> items;
	std::vector<Link> links;
	std::shared_ptr<Font> font, bold, heavy;

	void add(float x, float y, const std::string& t, Kind k = LABEL,
	         int align = NVG_ALIGN_CENTER) {
		items.push_back({Vec(x, y), t, k, align});
	}
	// the common cases, so a caller states the control position and nothing else
	void knob(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_KNOB, t); }
	void knobLarge(float x, float y, const std::string& t) { add(x, y - LABEL_GAP_KNOB_LARGE, t); }
	void rogan(float x, float y, const std::string& t) { add(x, y - LABEL_GAP_ROGAN, t); }
	void roganLarge(float x, float y, const std::string& t) { add(x, y - LABEL_GAP_ROGAN_LARGE, t); }
	void trim(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_TRIM, t); }
	void jack(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_JACK, t); }
	void jackOnPlate(float x, float y, const std::string& t) {
		add(x, y - LABEL_GAP_JACK, t, ON_PLATE);
	}
	void note(float x, float y, const std::string& t)  { add(x, y, t, NOTE); }

	// A pot and the jack that modulates it, tied together. The label goes on the
	// pot; the jack does not need one, because the line already says what it is.
	void pair(float x, float y, const std::string& t, bool onPlate = false) {
		add(x, y - LABEL_GAP_TRIM, t, onPlate ? ON_PLATE : LABEL);
		links.push_back({Vec(x, y), Vec(x + hp(PAIR_SPAN), y), onPlate});
	}
	// The same pairing turned through 90°: the jack sits BELOW its pot rather
	// than beside it, for panels that run their controls down a column. The
	// caller gives both centres, because the vertical gap is the row pitch and
	// that is a layout decision rather than a constant.
	void pairDown(float x, float y, float yJack, const std::string& t,
	              bool onPlate = false) {
		add(x, y - LABEL_GAP_TRIM, t, onPlate ? ON_PLATE : LABEL);
		links.push_back({Vec(x, y), Vec(x, yJack), onPlate});
	}
	void link(float x1, float y1, float x2, float y2, bool onPlate = false) {
		links.push_back({Vec(x1, y1), Vec(x2, y2), onPlate});
	}
	// A rule between sections. The designer strokes these exactly as the
	// connectors (RULE, RULE_W): horizontals run between the screen's edges,
	// about 2 mm below the controls above them; verticals part column groups,
	// starting just above their first label.
	void divider(float x1, float y1, float x2, float y2) {
		links.push_back({Vec(x1, y1), Vec(x2, y2), false});
	}
	// The module name: (x, baseline). title(t) places it where every finished
	// panel does -- set box.size first, since the margin depends on the width.
	void title(float x, float y, const std::string& t) {
		add(x, y, t, TITLE, NVG_ALIGN_LEFT);
	}
	void title(const std::string& t) {
		float wmm = box.size.x / mm2px(1.f);
		add(wmm <= 8.f * HP + 0.01f ? 0.5f * HP : HP, TITLE_BASELINE, t, TITLE, NVG_ALIGN_LEFT);
	}

	void draw(const DrawArgs& args) override {
		if (!font || font->handle < 0) font = panelFont();
		if (!bold || bold->handle < 0) bold = panelFontBold();
		if (!heavy || heavy->handle < 0) heavy = panelFontTitle();
		if (!font || font->handle < 0) return;
		NVGcontext* vg = args.vg;
		for (const Link& l : links) {          // drawn first, so a component covers its ends
			nvgBeginPath(vg);
			nvgMoveTo(vg, mm2px(l.a.x), mm2px(l.a.y));
			nvgLineTo(vg, mm2px(l.b.x), mm2px(l.b.y));
			nvgStrokeColor(vg, l.onPlate ? nvgRGB(0x4A, 0x4A, 0x52) : RULE);
			nvgStrokeWidth(vg, mm2px(RULE_W));
			nvgStroke(vg);
		}
		for (const Item& it : items) {
			bool isTitle = it.kind == TITLE;
			nvgFontFaceId(vg, isTitle && heavy && heavy->handle >= 0 ? heavy->handle : font->handle);
			nvgFontSize(vg, mm2px(NVG_EM * (it.kind == NOTE ? TYPE_NOTE
			                              : isTitle         ? TYPE_TITLE : TYPE_LABEL)));
			nvgTextLetterSpacing(vg, 0.f);
			nvgFillColor(vg, it.kind == ON_PLATE ? PLATE_INK
			               : it.kind == NOTE     ? INK_SOFT : INK);
			// every position is a BASELINE, as the designer's labels sit on one
			nvgTextAlign(vg, it.align | NVG_ALIGN_BASELINE);
			nvgText(vg, mm2px(it.pos.x), mm2px(it.pos.y), it.text.c_str(), NULL);
		}
		Widget::draw(args);
	}
};

}  // namespace sfs
