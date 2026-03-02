#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::resolveFont(const Style style) const { return getFont(style); }

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  const EpdFont* font = getFont(style);
  if (font) {
    font->getTextDimensions(string, w, h);
  } else {
    *w = 0;
    *h = 0;
  }
}

const EpdFontData* EpdFontFamily::getData(const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->data : nullptr;
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->getGlyph(cp) : nullptr;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->getKerning(leftCp, rightCp) : 0;
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->applyLigatures(cp, text) : cp;
}
