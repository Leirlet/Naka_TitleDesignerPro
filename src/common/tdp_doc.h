// NAKA Title Designer Pro - .tdp document model (v2: JSON)
#pragma once

#include "tdp_json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tdp {

// ---------------------------------------------------------------- paint
struct GradientStop {
	float pos = 0;             // 0..1
	uint32_t color = 0xFFFFFFFF; // ARGB
};

// Image or video (with alpha) laid over a shape. Videos are read through a cache of PNG frames made
// by ffmpeg (tdp_media.h); stills are read directly.
enum class TexFit { Cover, Contain, Stretch, Tile };
enum class TexArea { Text, Frame };   // what the texture is fitted to: the text block (moves with it) or the whole frame
enum class TexBlend { Normal, Multiply, Screen, Overlay, Add, Lighten, Darken, SoftLight };

struct Texture {
	std::wstring path;
	TexArea area = TexArea::Text;
	TexFit fit = TexFit::Cover;
	float scale = 100;             // %
	float offsetX = 0, offsetY = 0; // doc px
	float rotation = 0;            // degrees
	TexBlend blend = TexBlend::Normal;
	float opacity = 100;           // %
	float speed = 100;             // video: playback speed, %
	float startTime = 0;           // video: seconds into the clip at the first frame
	bool loop = true;              // video: repeat, otherwise hold the last frame
};

enum class PaintType { Solid, Linear, Radial, Stripe, Dots, Texture };

struct Paint {
	PaintType type = PaintType::Solid;
	uint32_t color = 0xFFFFFFFF;   // solid colour; stripe / dot ink colour
	uint32_t color2 = 0x00000000;  // stripe / dot background (may be transparent)
	float angle = 90;              // degrees. linear: direction (90 = top -> bottom); stripe / dots: pattern rotation
	std::vector<GradientStop> stops; // linear / radial
	float size = 10;               // stripe: stripe width; dots: dot diameter (doc px)
	float gap = 10;                // stripe: gap between stripes; dots: centre-to-centre spacing (doc px)
	// texture: `tex` is laid over `base` (any other paint; none = transparent underneath)
	Texture tex;
	std::shared_ptr<const Paint> base;
};

// ---------------------------------------------------------------- appearance stack
enum class LayerType { Fill, Stroke, Shadow, InnerShadow, LongShadow, Background, Extrude, Texture };
enum class StrokePosition { Outer, Center, Inner };
enum class LineJoin { Miter, Round, Bevel };
enum class BevelShape { None, Flat, Round, Cove };
// Fill / stroke surface. None = the flat 2D colour (a bevel only adds shading); Lit = the paint is
// the base colour of a lit material (metallic / roughness / brushed).
enum class Material { None, Lit };
// Surroundings reflected by lit surfaces: soft (the Phase 3 look) or a high-contrast studio for metal.
enum class EnvStyle { Soft, Studio };
// Turning of the 3D layer over time: constant spin, or swaying to and fro.
enum class TurnMode { None, Spin, Sway };

// One entry of the appearance stack. Index 0 is the front-most layer.
// Shadow / long shadow cast the combined shape of every layer in front of them.
struct Layer {
	std::string id;
	std::wstring name;
	LayerType type = LayerType::Fill;
	bool enabled = true;
	float opacity = 100;           // %

	Paint paint;                   // fill / stroke / background / long shadow

	// stroke
	float width = 8;
	StrokePosition position = StrokePosition::Outer;
	LineJoin join = LineJoin::Round;
	float miterLimit = 4;

	// shadow / inner shadow
	uint32_t color = 0xB4000000;
	float angle = 135;             // degrees, direction the shadow falls (0 = right, 90 = down)
	float distance = 8;
	float blur = 8;
	float spread = 0;              // shadow: grow before blur; inner shadow: choke

	// long shadow
	float length = 24;

	// background (rounded box around the text)
	float padX = 24, padY = 12, radius = 0;

	// 3D (extrude): extrudes the combined shape of the layers in front of it; their 2D look is the
	// front face. Only the first enabled 3D layer of a stack is drawn.
	float depth = 40;                    // doc px
	uint32_t sideColor = 0xFF9A9A9A;     // sides at the front
	uint32_t sideColor2 = 0xFF3C3C3C;    // sides at the back, back face
	// Bevel: 3D layer = real geometry; fill / stroke = shading from the distance to the layer's edge.
	BevelShape bevel = BevelShape::Round;
	float bevelWidth = 4, bevelHeight = 4; // doc px: inset from the edge / height (3D: depth taken)
	float rotX = 0, rotY = 0;            // degrees, whole object
	float perspective = 50;              // 0 = almost none .. 100 = strong
	// Turntable: added to rotX / rotY by the clip time. Spin = turnSpeed degrees per second; sway =
	// +- turnAmount degrees with a period of turnPeriod seconds. turnPhase: starting angle / phase.
	TurnMode turn = TurnMode::None;
	bool turnX = false;                  // around the horizontal axis instead of the vertical one
	float turnSpeed = 90, turnAmount = 25, turnPeriod = 3, turnPhase = 0;

	// Material: 3D layer sides (always lit), fill / stroke when material == Lit.
	Material material = Material::None;
	float metallic = 0;                  // %
	float roughness = 40;                // %
	bool brushed = false;                // hairline streaks (fill / stroke)

	// Bump (fill / stroke): the brightness of the texture is a height map for the surface shading.
	Texture bump;                        // empty path = none
	float bumpDepth = 4;                 // px of height for white (negative = sunk in)

	// Texture layer: laid over everything in front of it, blended; clip = only where those layers are.
	Texture tex;
	bool clip = true;
};

// Orbit = the light circles the text once per `period` seconds; Sweep = swings +- range/2 degrees
// around `angle` with that period; Pass = moves once from angle - range/2 to angle + range/2 during
// `duration` frames starting at frame `start`.
enum class LightMotion { None, Orbit, Sweep, Pass };

// Light shared by the 3D layer and lit / bevelled fills and strokes of one text object.
struct Light {
	float angle = 225;                   // degrees, where the light comes from (0 = right, 90 = down)
	float height = 45;                   // degrees above the text plane (90 = from the viewer)
	float intensity = 100;               // %
	float ambient = 35;                  // %
	EnvStyle env = EnvStyle::Soft;
	LightMotion motion = LightMotion::None;
	float range = 180;                   // degrees
	float period = 4;                    // seconds
	float start = 0, duration = 60;      // frames (pass)
};

// ---------------------------------------------------------------- animation
// Preset animation applied unit by unit (character / line / whole object), staggered in time.
// "in" plays from the clip start; "out" ends exactly at the end of the clip (Doc::durationFrames)
// and is the in-motion played backwards (so slide "from the left" leaves to the left).
// Flip3D: turns around the vertical (dir left / right) or horizontal (up / down) axis. Real 3D with
// a 3D layer in the stack, squashed flat otherwise.
// FlyZ: comes from behind (dir left) or from in front of (right) the text plane. Hinge: turns around
// the edge named by dir (left / right edge: a door, down edge: a domino standing up). Tumble: turns
// around both axes. Grow: the 3D layer's extrusion grows from flat.
enum class AnimType { None, Fade, Typewriter, Pop, Zoom, Drop, Rise, Slide, Spin, Flip3D, FlyZ, Hinge, Tumble, Grow };
enum class AnimUnit { Char, Line, All };
enum class AnimOrder { Forward, Backward, CenterOut, EdgesIn, Random };
enum class AnimEase { Auto, Linear, Out, InOut, Back, Bounce, Elastic };
enum class AnimDir { Left, Right, Up, Down };

struct AnimSpec {
	AnimType type = AnimType::None;
	AnimUnit unit = AnimUnit::Char;
	AnimOrder order = AnimOrder::Forward;
	AnimEase ease = AnimEase::Auto;   // Auto = the type's own curve
	AnimDir dir = AnimDir::Left;      // slide: where the units come from
	float duration = 12;              // frames each unit takes
	float stagger = 2;                // frames between consecutive units
	float delay = 0;                  // in: frames before the first unit starts; out: frames before the clip end
	float amount = 0;                 // 0 = type default. pop / zoom: start scale %, drop / rise / slide / flyZ: distance px, spin / flip3D / hinge / tumble: degrees
	bool fade = true;                 // fade the unit in together with the motion (types other than fade / typewriter)
	int seed = 1;                     // random order
};

// ---------------------------------------------------------------- text
// Metrics = the font's own kerning pairs (default); None = pairs off; Palt = proportional metrics
// (OpenType palt / vpal), the usual way to set Japanese tight.
enum class KernMode { Metrics, None, Palt };
enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

struct PaintOverride {
	std::string layerId;
	Paint paint;
};

// Per-character formatting over [start, start + length) in UTF-16 code units.
// Zero / empty values inherit from the text object.
struct Span {
	int start = 0, length = 0;
	std::wstring family;
	float size = 0;
	int weight = 0;
	float tracking = 0;            // added after each character (doc px)
	float baselineShift = 0;       // + = up (doc px)
	bool tcy = false;              // vertical text: set horizontally in one character cell (tate-chu-yoko)
	// Ruby (furigana) over the characters of the span. Consecutive characters with the same ruby
	// form one group: the ruby is centred over the whole group.
	std::wstring ruby;
	std::vector<PaintOverride> paints; // per-layer paint replacements
};

struct TextObject {
	std::string id;
	std::wstring name;
	bool visible = true;
	std::wstring text = L"テロップ";

	// Transform. (x, y) is where the anchor of the text block lands.
	float x = 960, y = 540;
	HAlign align = HAlign::Center;   // also the horizontal anchor
	VAlign valign = VAlign::Middle;  // vertical anchor
	float rotation = 0, scaleX = 100, scaleY = 100, opacity = 100;

	std::wstring family = L"Yu Gothic UI";
	float size = 120;
	int weight = 700;
	bool italic = false;
	float tracking = 0;              // doc px after each character
	float lineHeight = 0;            // 0 = font default
	// Vertical writing (columns right to left). align then works along the column (left = top,
	// right = bottom) and is also the vertical anchor; valign picks the horizontal anchor
	// (top = right edge, middle = centre, bottom = left edge).
	bool vertical = false;
	// Tate-chu-yoko: runs of half-width digits up to this many (and "!!" / "!?" pairs) are set
	// horizontally in one cell. 0 = only the ranges marked in spans.
	int tcyDigits = 2;
	// Characters joined to an automatic digit group when they touch it, e.g. "()" makes "(15)" one group.
	std::wstring tcyChars = L"()";
	bool paintPerLine = true;        // gradients / patterns span each line (column) on its own, not the whole block
	float rubyScale = 50;            // ruby size, % of its base characters
	float rubyGap = 0;               // extra distance between ruby and base (doc px)

	std::vector<Span> spans;
	std::vector<std::pair<int, float>> kerning; // (char index, extra spacing after it)
	KernMode kernMode = KernMode::Metrics;
	float opticalKern = 0;           // % of automatic optical spacing (evens out the visible gaps), 0 = off

	std::vector<Layer> stack;
	Light light;

	AnimSpec animIn, animOut;
};

struct Doc {
	int version = 2;
	int width = 1920;
	int height = 1080;
	int vidScale = 30000;            // vidScale / vidSampleSize = fps
	int vidSampleSize = 1001;
	int durationFrames = 150;
	std::vector<TextObject> objects; // drawn first to last (last = top)
};

// ---------------------------------------------------------------- helpers
std::string NewId(const char* prefix);
Layer MakeLayer(LayerType type);
TextObject MakeTextObject(const Doc& doc);
Doc MakeDefaultDoc(int width, int height, int vidScale, int vidSampleSize);

float ObjectOpacity(const TextObject& obj); // 0..1

// Animated state of one unit (character / line / object) at a frame. Offsets in doc px,
// scale as a factor, rotation in degrees, around the unit's centre. rotX / rotY: 3D turn in degrees.
struct UnitState {
	float dx = 0, dy = 0, scale = 1, rotation = 0, opacity = 1;
	float rotX = 0, rotY = 0;
	float dz = 0;                    // doc px away from the viewer
	float depth = 1;                 // factor on the 3D layer's extrusion depth
	float px = 0, py = 0;            // pivot of scale / rotation, from the unit centre in unit sizes (+x right, +y down)
};
// State of one unit for one animation (in or out). index = position of the unit in reading order,
// count = number of units. In and out states are combined by the renderer.
UnitState EvalAnim(const AnimSpec& a, bool out, int index, int count, int frame, int durationFrames);
bool HasAnimation(const TextObject& obj);
// Light of the text at a frame (the motion applied to angle).
Light AnimatedLight(const Light& l, int frame, float fps);
// Turntable rotation to add to a 3D layer's rotX / rotY at a frame.
void TurnOf(const Layer& l, int frame, float fps, float& dRotX, float& dRotY);
// Frame of a texture (video) to show at a doc frame: index in [0, count).
int TextureFrameIndex(const Texture& t, int frame, float fps, int count, double texFps);
// Index of the 3D layer that is drawn (first enabled one), or -1.
int Extrude3DIndex(const TextObject& obj);
// Fill / stroke drawn with the surface pass (bevel or lit material).
bool HasSurface(const Layer& l);

// Tate-chu-yoko groups (start, length) of a vertical text object, in text order. Empty for horizontal text.
std::vector<std::pair<int, int>> TcyRanges(const TextObject& obj);

json::Value TextureToJson(const Texture& t);
Texture TextureFromJson(const json::Value& v);
json::Value PaintToJson(const Paint& p);
Paint PaintFromJson(const json::Value& v);
json::Value LayerToJson(const Layer& l);
Layer LayerFromJson(const json::Value& v);
json::Value DocToJson(const Doc& doc);
bool DocFromJson(const json::Value& v, Doc& out);

bool LoadDoc(const std::wstring& path, Doc& out);   // v2 JSON, or v1 key=value (converted)
bool SaveDoc(const std::wstring& path, const Doc& doc);

bool ReadFileUtf8(const std::wstring& path, std::string& out);
bool WriteFileUtf8(const std::wstring& path, const std::string& data);

std::string ColorToString(uint32_t argb);            // "#AARRGGBB"
uint32_t ColorFromString(const std::string& s, uint32_t fallback);

} // namespace tdp
