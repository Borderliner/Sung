#pragma once
// Material 3 dynamic color.
//
// A cover supplies one source color; Material derives the whole interface from
// it. The path is the one the Material 3 guidelines describe: measure the source
// in HCT (CAM16 hue and chroma over CIE L* tone), spread five tonal palettes
// around it, then read the color roles off those palettes at fixed tones.
//
// Tone is L*, so the tone numbers the guidelines quote are what carry the
// contrast guarantees. This solver reproduces the requested tone and hue
// exactly and approaches the requested chroma from above, stepping down until
// the color fits inside sRGB. Material's own solver walks the gamut boundary
// analytically to land on the last representable chroma; stepping finds the
// same tone and hue with chroma within a fraction of a unit, which no eye and
// no contrast check can separate.
#include <QColor>
#include <QHash>
#include <QVariantMap>

namespace m3 {

// A color measured in HCT. Hue is degrees, chroma is unbounded in principle and
// reaches about 120 inside sRGB, tone is CIE L* from 0 to 100.
struct Hct {
  double hue = 0;
  double chroma = 0;
  double tone = 0;
};

Hct measure(const QColor &color);
// The sRGB color closest to this hue and chroma at exactly this tone.
QColor solve(double hue, double chroma, double tone);

// One Material tonal palette: a fixed hue and chroma read at any tone.
struct TonalPalette {
  double hue = 0;
  double chroma = 0;
  QColor tone(double value) const { return solve(hue, chroma, value); }
};

// The five palettes Material's default scheme spreads around a source color.
struct Palettes {
  TonalPalette primary, secondary, tertiary, neutral, neutralVariant;
};
Palettes palettesFor(const QColor &source);

// Every color role the interface uses, keyed by its Material name.
QVariantMap scheme(const QColor &source, bool dark);

// L* of a color, on the same 0-100 scale as tone.
double toneOf(const QColor &color);

} // namespace m3
