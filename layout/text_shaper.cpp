#include "text_shaper.h"
#include "layout.h"
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

namespace browser {

namespace {

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------

// Decodes the codepoint starting at byte i. len receives the byte length
// (1 on malformed input so scanning always advances).
inline uint32_t decodeCp(const std::string& s, size_t i, int& len) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { len = 1; return c; }
    int n = 0; uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else { len = 1; return 0xFFFD; }
    if (i + n > s.size()) { len = 1; return 0xFFFD; }
    for (int k = 1; k < n; ++k) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { len = 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    len = n;
    return cp;
}

inline void encodeCp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Bidi character classes (simplified UAX#9)
// ---------------------------------------------------------------------------

enum BC {
    BC_L,    // strong left-to-right
    BC_R,    // strong right-to-left (Hebrew &c.)
    BC_AL,   // Arabic letter (strong RTL, joins)
    BC_EN,   // European number
    BC_AN,   // Arabic-Indic number
    BC_NSM,  // non-spacing mark (combining)
    BC_ET,   // European terminator ($ % #)
    BC_ES,   // European separator (+ -)
    BC_CS,   // common separator (, . : /)
    BC_N     // neutral (spaces, punctuation, symbols, unknown)
};

inline BC classify(uint32_t cp) {
    if (cp < 0x80) {
        if (cp >= '0' && cp <= '9') return BC_EN;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return BC_L;
        switch (cp) {
            case '$': case '%': case '#': return BC_ET;
            case '+': case '-': return BC_ES;
            case ',': case '.': case ':': case '/': return BC_CS;
            default: return BC_N;
        }
    }
    // Hebrew block (+ Hebrew presentation forms)
    if ((cp >= 0x0590 && cp <= 0x05BF) || (cp >= 0x05C1 && cp <= 0x05C2) ||
        cp == 0x05C7 || (cp >= 0x05D0 && cp <= 0x05EA) ||
        (cp >= 0x05EF && cp <= 0x05F4) || (cp >= 0xFB1D && cp <= 0xFB4F))
        return BC_R;
    // Arabic block
    if (cp >= 0x0600 && cp <= 0x06FF) {
        if (cp >= 0x0660 && cp <= 0x0669) return BC_AN;
        if (cp >= 0x06F0 && cp <= 0x06F9) return BC_EN;
        if ((cp >= 0x064B && cp <= 0x0655) || cp == 0x0670 ||
            (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
            (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED))
            return BC_NSM;
        return BC_AL;   // letters, tatweel, Arabic punctuation
    }
    // Arabic presentation forms (already shaped — pass through as RTL)
    if ((cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF))
        return BC_AL;
    // Other RTL scripts (Syriac, Thaana, NKo, ...)
    if ((cp >= 0x0700 && cp <= 0x074F) || (cp >= 0x0780 && cp <= 0x07BF) ||
        (cp >= 0x07C0 && cp <= 0x07FF) || (cp >= 0x0800 && cp <= 0x083F) ||
        (cp >= 0x08A0 && cp <= 0x08FF))
        return BC_R;
    // Explicit direction marks
    if (cp == 0x200F) return BC_R;   // RLM
    if (cp == 0x200E) return BC_L;   // LRM
    // Fullwidth digits
    if (cp >= 0xFF10 && cp <= 0xFF19) return BC_EN;
    // Known LTR scripts (Greek, Cyrillic, CJK, Hangul, kana, Latin ext.)
    if ((cp >= 0x00C0 && cp <= 0x02AF) || (cp >= 0x0370 && cp <= 0x052F) ||
        (cp >= 0x1E00 && cp <= 0x1FFF) || (cp >= 0x2C60 && cp <= 0x2C7F) ||
        (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3130 && cp <= 0x318F) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7FF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A) ||
        (cp >= 0xFF66 && cp <= 0xFFDC))
        return BC_L;
    // Combining marks for Latin/Greek/etc — behave like NSM
    if (cp >= 0x0300 && cp <= 0x036F) return BC_NSM;
    return BC_N;
}

inline bool isDigitCp(uint32_t cp) {
    return (cp >= '0' && cp <= '9') ||
           (cp >= 0x0660 && cp <= 0x0669) ||
           (cp >= 0x06F0 && cp <= 0x06F9) ||
           (cp >= 0xFF10 && cp <= 0xFF19);
}

// ---------------------------------------------------------------------------
// Arabic joining + presentation forms
// ---------------------------------------------------------------------------

enum Join { J_U = 0, J_R = 1, J_D = 2, J_T = 3 };  // U, right, dual, tatweel

struct LetterForms {
    uint16_t iso;   // isolated presentation form (0 = keep original cp)
    uint8_t  join;  // Join
};

// 0x0621..0x064A — indexed by cp - 0x0621. The Unicode presentation-forms
// table is laid out [iso][fin][ini][med] for dual-joining letters and
// [iso][fin] for right-joining letters, so every form is iso + offset.
const LetterForms kLetters[] = {
    {0xFE80, J_U},  // 0621 HAMZA
    {0xFE81, J_R},  // 0622 ALEF MADDA
    {0xFE83, J_R},  // 0623 ALEF HAMZA ABOVE
    {0xFE85, J_R},  // 0624 WAW HAMZA
    {0xFE87, J_R},  // 0625 ALEF HAMZA BELOW
    {0xFE89, J_D},  // 0626 YEH HAMZA
    {0xFE8D, J_R},  // 0627 ALEF
    {0xFE8F, J_D},  // 0628 BEH
    {0xFE93, J_R},  // 0629 TEH MARBUTA
    {0xFE95, J_D},  // 062A TEH
    {0xFE99, J_D},  // 062B THEH
    {0xFE9D, J_D},  // 062C JEEM
    {0xFEA1, J_D},  // 062D HAH
    {0xFEA5, J_D},  // 062E KHAH
    {0xFEA9, J_R},  // 062F DAL
    {0xFEAB, J_R},  // 0630 THAL
    {0xFEAD, J_R},  // 0631 REH
    {0xFEAF, J_R},  // 0632 ZAIN
    {0xFEB1, J_D},  // 0633 SEEN
    {0xFEB5, J_D},  // 0634 SHEEN
    {0xFEB9, J_D},  // 0635 SAD
    {0xFEBD, J_D},  // 0636 DAD
    {0xFEC1, J_D},  // 0637 TAH
    {0xFEC5, J_D},  // 0638 ZAH
    {0xFEC9, J_D},  // 0639 AIN
    {0xFECD, J_D},  // 063A GHAIN
    {0,      J_U},  // 063B
    {0,      J_U},  // 063C
    {0,      J_U},  // 063D
    {0,      J_U},  // 063E
    {0,      J_U},  // 063F
    {0,      J_T},  // 0640 TATWEEL (joins both sides, no form change)
    {0xFED1, J_D},  // 0641 FEH
    {0xFED5, J_D},  // 0642 QAF
    {0xFED9, J_D},  // 0643 KAF
    {0xFEDD, J_D},  // 0644 LAM
    {0xFEE1, J_D},  // 0645 MEEM
    {0xFEE5, J_D},  // 0646 NOON
    {0xFEE9, J_D},  // 0647 HEH
    {0xFEED, J_R},  // 0648 WAW
    {0xFEEF, J_R},  // 0649 ALEF MAKSURA
    {0xFEF1, J_D},  // 064A YEH
};

inline const LetterForms* arabicLetter(uint32_t cp) {
    if (cp >= 0x0621 && cp <= 0x064A) return &kLetters[cp - 0x0621];
    return nullptr;
}

inline bool isArabicMark(uint32_t cp) {
    return (cp >= 0x064B && cp <= 0x0655) || cp == 0x0670 ||
           (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
           (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED) ||
           (cp >= 0x0300 && cp <= 0x036F);
}

// Bracket mirroring inside RTL runs (UAX#9 rule L4).
inline uint32_t mirrorCp(uint32_t cp) {
    switch (cp) {
        case '(': return ')';  case ')': return '(';
        case '[': return ']';  case ']': return '[';
        case '{': return '}';  case '}': return '{';
        case '<': return '>';  case '>': return '<';
        case 0x2039: return 0x203A;  case 0x203A: return 0x2039;
        case 0x00AB: return 0x00BB;  case 0x00BB: return 0x00AB;
        default: return cp;
    }
}

// Would a lam at index i ligate with the (adjacent) alef at i+1?
inline bool lamAlefPair(uint32_t lam, uint32_t next) {
    return lam == 0x0644 && (next == 0x0622 || next == 0x0623 ||
                             next == 0x0625 || next == 0x0627);
}

// Mark the second half of every directly-adjacent lam-alef pair.
inline void markLamAlefConsumed(const std::vector<uint32_t>& raw,
                                std::vector<uint8_t>& consumed) {
    for (size_t k = 0; k + 1 < raw.size(); ++k) {
        if (lamAlefPair(raw[k], raw[k + 1])) {
            consumed[k + 1] = 1;
            ++k;  // the alef cannot start another pair
        }
    }
}

// Shape one logical RTL segment (only R/AL chars, numbers, neutrals and
// embedded LTR digits appear here). `isNum[i]` marks number-class chars
// (EN/AN incl. separators/terminators resolved to numbers by the W rules)
// — maximal number runs form atomic clusters that stay forward. Returns
// the visual string — draw it left-to-right with the normal renderer.
std::string shapeRTLLogicalImpl(const std::vector<uint32_t>& in,
                                const std::vector<uint8_t>& isNum) {
    size_t n = in.size();
    if (n == 0) return {};

    // ---- joining analysis over logical order (marks transparent) ----
    std::vector<uint8_t> linkPrev(n, 0), linkNext(n, 0);
    {
        int last = -1;   // index of last joinable-or-blocking char
        for (size_t i = 0; i < n; ++i) {
            const LetterForms* lf = arabicLetter(in[i]);
            bool joinable = lf || in[i] == 0x0640;
            if (last >= 0 && joinable && lf && lf->join != J_U) {
                const LetterForms* pf = arabicLetter(in[last]);
                uint8_t pj = pf ? (uint8_t)pf->join : (uint8_t)J_U;
                if (in[last] == 0x0640) pj = J_T;
                if (pj == J_D || pj == J_T) linkPrev[i] = 1;
            }
            if (joinable || !isArabicMark(in[i])) last = (int)i;
        }
        last = -1;
        for (size_t ii = n; ii-- > 0;) {
            const LetterForms* lf = arabicLetter(in[ii]);
            bool joinable = lf || in[ii] == 0x0640;
            if (last >= 0 && joinable && lf &&
                (lf->join == J_D || lf->join == J_T)) {
                const LetterForms* nf = arabicLetter(in[last]);
                uint8_t nj = nf ? (uint8_t)nf->join : (uint8_t)J_U;
                if (in[last] == 0x0640) nj = J_T;
                if (nj == J_R || nj == J_D || nj == J_T) linkNext[ii] = 1;
            }
            if (joinable || !isArabicMark(in[ii])) last = (int)ii;
        }
    }

    // ---- presentation-form substitution + lam-alef ligatures ----
    std::vector<uint32_t> shaped(n);
    for (size_t i = 0; i < n; ++i) shaped[i] = in[i];

    for (size_t i = 0; i < n; ++i) {
        if (shaped[i] == 0) continue;        // consumed by a lam-alef ligature
        uint32_t cp = in[i];
        if (lamAlefPair(cp, i + 1 < n ? in[i + 1] : 0)) {
            // Only ligate when the alef is DIRECTLY adjacent (marks in
            // between are a rare corner case — fall back to plain forms).
            uint32_t lig;
            switch (in[i + 1]) {
                case 0x0622: lig = 0xFEF5; break;
                case 0x0623: lig = 0xFEF7; break;
                case 0x0625: lig = 0xFEF9; break;
                default:     lig = 0xFEFB; break;
            }
            shaped[i] = linkPrev[i] ? lig + 1 : lig;  // final / isolated
            shaped[i + 1] = 0;                        // consumed
            continue;
        }
        const LetterForms* lf = arabicLetter(cp);
        if (!lf || lf->iso == 0) continue;   // tatweel / extended / marks
        bool lp = linkPrev[i] != 0;
        bool ln = linkNext[i] != 0;
        int form;
        if (lf->join == J_D)
            form = (lp && ln) ? 3 : (ln ? 2 : (lp ? 1 : 0));
        else
            form = lp ? 1 : 0;               // right-joining: iso/fin only
        shaped[i] = lf->iso + form;
    }

    // ---- visual assembly: reverse unit order, number clusters forward ----
    // A "unit" is one char or one maximal number cluster; marks land
    // before their base naturally because reversal visits them first.
    std::string out;
    out.reserve(n * 2 + 4);
    size_t i = n;
    while (i-- > 0) {
        if (shaped[i] == 0) {                // ligature remainder
            if (i == 0) break;
            continue;
        }
        if (isNum[i]) {
            size_t start = i;
            while (start > 0 && isNum[start - 1]) --start;
            for (size_t k = start; k <= i; ++k)   // numbers read LTR
                encodeCp(out, shaped[k]);
            if (start == 0) break;
            i = start;                            // loop decrements
            continue;
        }
        encodeCp(out, mirrorCp(shaped[i]));
        if (i == 0) break;
    }
    return out;
}

// Number mask for the standalone entry point: classify + the W rules
// that grow number runs (W4 separators, W5 terminators).
std::vector<uint8_t> numberMask(const std::vector<uint32_t>& cps) {
    size_t n = cps.size();
    std::vector<BC> cls(n);
    for (size_t i = 0; i < n; ++i) cls[i] = classify(cps[i]);
    for (size_t i = 0; i < n; ++i) {
        if (cls[i] == BC_CS && i && i + 1 < n &&
            cls[i - 1] == BC_EN && cls[i + 1] == BC_EN) cls[i] = BC_EN;
        else if (cls[i] == BC_CS && i && i + 1 < n &&
                 cls[i - 1] == BC_AN && cls[i + 1] == BC_AN) cls[i] = BC_AN;
        else if (cls[i] == BC_ES && i && i + 1 < n &&
                 cls[i - 1] == BC_EN && cls[i + 1] == BC_EN) cls[i] = BC_EN;
    }
    for (size_t i = 0; i < n; ++i) {
        if (cls[i] == BC_ET &&
            ((i && cls[i - 1] == BC_EN) ||
             (i + 1 < n && cls[i + 1] == BC_EN)))
            cls[i] = BC_EN;
    }
    std::vector<uint8_t> m(n, 0);
    for (size_t i = 0; i < n; ++i)
        m[i] = (cls[i] == BC_EN || cls[i] == BC_AN) ? 1 : 0;
    return m;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

bool containsRTL(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size();) {
        int len = 0;
        uint32_t cp = decodeCp(utf8, i, len);
        i += (size_t)len;
        if (cp >= 0x80) {
            BC c = classify(cp);
            if (c == BC_R || c == BC_AL) return true;
        }
    }
    return false;
}

bool detectRTL(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size();) {
        int len = 0;
        uint32_t cp = decodeCp(utf8, i, len);
        i += (size_t)len;
        if (cp < 0x80) {
            if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
                return false;   // first strong char is LTR
            continue;
        }
        BC c = classify(cp);
        if (c == BC_R || c == BC_AL) return true;
        if (c == BC_L) return false;
    }
    return false;
}

std::string shapeRTLLogical(const std::string& utf8) {
    std::vector<uint32_t> cps;
    cps.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        int len = 0;
        cps.push_back(decodeCp(utf8, i, len));
        i += (size_t)len;
    }
    return shapeRTLLogicalImpl(cps, numberMask(cps));
}

// ---------------------------------------------------------------------------
// Line-level bidi reordering
// ---------------------------------------------------------------------------

void applyBidiToLine(std::vector<Run>& line, bool baseRTL) {
    if (line.empty()) return;

    // Fast path: nothing above ASCII → logical order is already visual.
    bool anyHigh = false;
    for (const auto& r : line) {
        for (unsigned char ch : r.text)
            if (ch >= 0x80) { anyHigh = true; break; }
        if (anyHigh) break;
    }
    if (!anyHigh) return;

    // Decode all runs into one logical char list. Image runs become
    // pseudo-chars (cp 0) so they participate in reordering as units.
    struct Ch {
        uint32_t cp;        // current (possibly substituted) codepoint
        uint32_t o1, o2;    // original codepoint(s) behind this char
        int      run;       // index into line[] this char came from
        size_t   lidx;      // logical index in the original line
        BC       cls;
        bool     image;
        bool     rtlSrc;    // emitted from an RTL (shaped) segment
    };
    std::vector<Ch> cs;
    cs.reserve(line.size() * 4 + 16);
    for (int ri = 0; ri < (int)line.size(); ++ri) {
        const Run& r = line[ri];
        if (r.isImage) {
            Ch c{}; c.run = ri; c.lidx = cs.size(); c.image = true;
            cs.push_back(c);
            continue;
        }
        for (size_t i = 0; i < r.text.size();) {
            int len = 0;
            uint32_t cp = decodeCp(r.text, i, len);
            Ch c{};
            c.cp = cp; c.o1 = cp; c.run = ri; c.lidx = cs.size();
            c.cls = classify(cp);
            cs.push_back(c);
            i += (size_t)len;
        }
    }
    size_t n = cs.size();
    if (n == 0) return;

    // ---- W1: NSM takes the class of the previous char ----
    for (size_t i = 0; i < n; ++i) {
        if (cs[i].cls == BC_NSM)
            cs[i].cls = i ? cs[i - 1].cls : (baseRTL ? BC_R : BC_L);
    }
    // ---- W2: EN after AL becomes AN ----
    {
        BC lastStrong = baseRTL ? BC_R : BC_L;
        for (size_t i = 0; i < n; ++i) {
            if (cs[i].cls == BC_EN && lastStrong == BC_AL) cs[i].cls = BC_AN;
            if (cs[i].cls == BC_AL || cs[i].cls == BC_R || cs[i].cls == BC_L)
                lastStrong = cs[i].cls;
        }
    }
    // ---- W3: AL → R ----
    for (auto& c : cs)
        if (c.cls == BC_AL) c.cls = BC_R;
    // ---- W4/W5: separators between numbers, terminators beside numbers ----
    for (size_t i = 0; i < n; ++i) {
        if (cs[i].cls == BC_CS && i && i + 1 < n &&
            cs[i - 1].cls == BC_EN && cs[i + 1].cls == BC_EN) cs[i].cls = BC_EN;
        else if (cs[i].cls == BC_CS && i && i + 1 < n &&
                 cs[i - 1].cls == BC_AN && cs[i + 1].cls == BC_AN) cs[i].cls = BC_AN;
        else if (cs[i].cls == BC_ES && i && i + 1 < n &&
                 cs[i - 1].cls == BC_EN && cs[i + 1].cls == BC_EN) cs[i].cls = BC_EN;
    }
    for (size_t i = 0; i < n; ++i) {
        if (cs[i].cls == BC_ET &&
            ((i && cs[i - 1].cls == BC_EN) ||
             (i + 1 < n && cs[i + 1].cls == BC_EN)))
            cs[i].cls = BC_EN;
    }
    // ---- W7: EN after L becomes L ----
    {
        BC lastStrong = baseRTL ? BC_R : BC_L;
        for (size_t i = 0; i < n; ++i) {
            if (cs[i].cls == BC_EN && lastStrong == BC_L) cs[i].cls = BC_L;
            if (cs[i].cls == BC_R || cs[i].cls == BC_L) lastStrong = cs[i].cls;
        }
    }
    // ---- N1/N2: resolve remaining neutrals to a side ----
    {
        auto isLside = [](BC c) { return c == BC_L || c == BC_EN; };
        auto isRside = [](BC c) { return c == BC_R || c == BC_AN || c == BC_EN; };
        size_t i = 0;
        while (i < n) {
            if (cs[i].cls != BC_N && cs[i].cls != BC_ET &&
                cs[i].cls != BC_ES && cs[i].cls != BC_CS) { ++i; continue; }
            size_t j = i;
            while (j < n && (cs[j].cls == BC_N || cs[j].cls == BC_ET ||
                             cs[j].cls == BC_ES || cs[j].cls == BC_CS)) ++j;
            BC left  = i ? cs[i - 1].cls : (baseRTL ? BC_R : BC_L);
            BC right = (j < n) ? cs[j].cls : (baseRTL ? BC_R : BC_L);
            bool lL = isLside(left), lR = isRside(left);
            bool rL = isLside(right), rR = isRside(right);
            BC chosen;
            if (lL && rL) chosen = BC_L;
            else if (lR && rR) chosen = BC_R;
            else if (lL || rL) chosen = BC_L;      // one-sided → that side
            else if (lR || rR) chosen = BC_R;
            else chosen = baseRTL ? BC_R : BC_L;
            for (size_t k = i; k < j; ++k) cs[k].cls = chosen;
            i = j;
        }
    }

    // ---- assign each char to the L (0) or RTL (1) side ----
    // Strong chars decide; numbers/neutrals/images take the nearest
    // strong side (ties and orphans → base direction).
    std::vector<int> kind(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (cs[i].image) continue;
        if (cs[i].cls == BC_L) kind[i] = 0;
        else if (cs[i].cls == BC_R) kind[i] = 1;
    }
    for (size_t i = 0; i < n; ++i) {
        if (kind[i] != -1) continue;
        int left = -1, right = -1;
        for (size_t k = i; k-- > 0;)
            if (kind[k] != -1) { left = kind[k]; break; }
        for (size_t k = i + 1; k < n; ++k)
            if (kind[k] != -1) { right = kind[k]; break; }
        if (left >= 0 && (right < 0 || left == right)) kind[i] = left;
        else if (right >= 0 && left < 0)               kind[i] = right;
        else                                           kind[i] = baseRTL ? 1 : 0;
    }

    // ---- build segments (maximal same-kind runs) ----
    struct Seg { int kind; size_t a, b; };  // [a, b)
    std::vector<Seg> segs;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && kind[j] == kind[i]) ++j;
        segs.push_back(Seg{kind[i], i, j});
        i = j;
    }

    // ---- emit visual order ----
    std::vector<Ch> vis;
    vis.reserve(n);
    auto emitSegLTR = [&](const Seg& sg) {
        for (size_t k = sg.a; k < sg.b; ++k) vis.push_back(cs[k]);
    };
    auto emitSegRTL = [&](const Seg& sg) {
        std::vector<uint32_t> raw;
        std::vector<uint8_t> numMask;
        raw.reserve(sg.b - sg.a);
        numMask.reserve(sg.b - sg.a);
        for (size_t k = sg.a; k < sg.b; ++k) {
            raw.push_back(cs[k].cp);
            numMask.push_back(
                (cs[k].cls == BC_EN || cs[k].cls == BC_AN) ? 1 : 0);
        }
        std::vector<uint8_t> consumed(raw.size(), 0);
        markLamAlefConsumed(raw, consumed);

        std::string shaped = shapeRTLLogicalImpl(raw, numMask);
        std::vector<uint32_t> scps;
        for (size_t p = 0; p < shaped.size();) {
            int len = 0;
            scps.push_back(decodeCp(shaped, p, len));
            p += (size_t)len;
        }
        // Replay the shaper's unit walk to map emitted chars back to raw
        // indices (number clusters forward, consumed alefs skipped).
        std::vector<size_t> emitIdx;
        size_t ii = raw.size();
        while (ii-- > 0) {
            if (consumed[ii]) {
                if (ii == 0) break;
                continue;
            }
            if (numMask[ii]) {
                size_t start = ii;
                while (start > 0 && numMask[start - 1]) --start;
                for (size_t k = start; k <= ii; ++k)
                    if (!consumed[k]) emitIdx.push_back(k);
                if (start == 0) break;
                ii = start;                       // loop decrements
                continue;
            }
            emitIdx.push_back(ii);
            if (ii == 0) break;
        }
        size_t si = 0;
        for (size_t e = 0; e < emitIdx.size() && si < scps.size(); ++e) {
            size_t ridx = emitIdx[e];
            Ch c = cs[sg.a + ridx];
            c.cp = scps[si++];
            c.rtlSrc = true;
            // lam-alef ligature: attach the consumed alef's origin
            if (ridx + 1 < raw.size() && consumed[ridx + 1])
                c.o2 = cs[sg.a + ridx + 1].o1;
            vis.push_back(c);
        }
    };

    if (baseRTL) {
        for (size_t s = segs.size(); s-- > 0;) {
            if (segs[s].kind == 1) emitSegRTL(segs[s]);
            else emitSegLTR(segs[s]);
        }
    } else {
        for (auto& sg : segs) {
            if (sg.kind == 1) emitSegRTL(sg);
            else emitSegLTR(sg);
        }
    }

    // ---- rebuild runs: group contiguous visual chars by source run ----
    std::vector<Run> out;
    out.reserve(line.size());
    size_t i = 0;
    while (i < vis.size()) {
        int ri = vis[i].run;
        bool isImg = vis[i].image;
        size_t j = i;
        std::string vtxt;
        struct Orig { size_t lidx; uint32_t o1, o2; };
        std::vector<Orig> origs;
        while (j < vis.size() && vis[j].run == ri && vis[j].image == isImg) {
            if (isImg) { ++j; break; }   // image pseudo-char: run as-is
            encodeCp(vtxt, vis[j].cp);
            origs.push_back(Orig{vis[j].lidx, vis[j].o1, vis[j].o2});
            ++j;
        }
        if (isImg) {
            out.push_back(line[ri]);     // image run untouched
        } else if (!vtxt.empty()) {
            Run r = line[ri];
            r.text = vtxt;
            // Logical text = the original codepoints in logical order
            // (visual order may be reversed within RTL segments).
            std::sort(origs.begin(), origs.end(),
                      [](const Orig& a, const Orig& b) { return a.lidx < b.lidx; });
            std::string ltxt;
            for (auto& o : origs) {
                encodeCp(ltxt, o.o1);
                if (o.o2) encodeCp(ltxt, o.o2);
            }
            if (ltxt != vtxt) r.logical = ltxt;
            out.push_back(r);
        }
        i = j;
    }
    line.swap(out);
}

} // namespace browser
