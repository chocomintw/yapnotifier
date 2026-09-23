"""Shrink upstream InterVariable.ttf to what the overlay can actually draw.

    python tools/subset_font.py InterVariable.ttf assets/InterVariable.ttf   (needs: pip install fonttools)

RmlUi renders through FreeType with no shaping engine, so OpenType layout (GSUB/GPOS/GDEF:
alternates, ligatures, GPOS kerning) and the unencoded / private-use glyphs only reachable
through it are never used. Every named instance sits at opsz=14, so pinning that axis keeps
those instances identical and drops its variation data; the wght axis stays variable.
Kept: Latin (+ extensions, Vietnamese), IPA, Greek, Cyrillic, punctuation, symbols.
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

UNICODES = [
    (0x0000, 0x036F),  # Latin, Latin-1, Extended-A/B, IPA, spacing modifiers, combining marks
    (0x0370, 0x03FF),  # Greek
    (0x0400, 0x052F),  # Cyrillic + supplement
    (0x1E00, 0x1EFF),  # Latin Extended Additional (Vietnamese)
    (0x2000, 0x27BF),  # punctuation, currency, letterlike, arrows, math, shapes, symbols, dingbats
    (0xFFFD, 0xFFFD),  # replacement character
]


def main(src: str, dst: str) -> None:
    font = TTFont(src)
    opts = subset.Options()
    opts.layout_features = []
    opts.drop_tables += ["GSUB", "GPOS", "GDEF"]
    opts.glyph_names = False
    opts.hinting = False
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=[cp for lo, hi in UNICODES for cp in range(lo, hi + 1)])
    sub.subset(font)
    # Subset first: the subsetter trips over an in-memory instanced font's lazy gvar.
    font = instancer.instantiateVariableFont(font, {"opsz": 14})
    font.save(dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
