/* Ledger.cpp -- ACCESSION.
 *
 * The reasoning for all of this is in Ledger.h. What is here is the mechanism:
 * the serial mint, the line codec, the crash-safe append, the parse, and the
 * graph walks that turn a pile of files into a credits sheet.
 */

#include "Ledger.h"

#include "Session.h"                    // morgueDir() -- never rebuild the path

#include <juce_cryptography/juce_cryptography.h>   // juce::SHA256
#include <juce_events/juce_events.h>               // MessageManager::callAsync

#include <algorithm>
#include <atomic>
#include <random>

namespace morgue
{

/* ==========================================================================
 *  KIND
 * ========================================================================== */
const char* kindTag (Kind k) noexcept
{
    switch (k)
    {
        case Kind::Spc:  return "SPC";
        case Kind::Clip: return "CLIP";
        case Kind::Rec:  return "REC";
        case Kind::Acq:  return "ACQ";
        case Kind::Scn:  return "SCN";
        case Kind::Plt:  return "PLT";
        case Kind::Rcp:  return "RCP";
        case Kind::Exp:  return "EXP";
        default:         return "UNK";
    }
}

Kind kindFromTag (juce::StringRef t) noexcept
{
    const juce::String s = juce::String (t).trim().toUpperCase();
    if (s == "SPC")  return Kind::Spc;
    if (s == "CLIP") return Kind::Clip;
    if (s == "REC")  return Kind::Rec;
    if (s == "ACQ")  return Kind::Acq;
    if (s == "SCN")  return Kind::Scn;
    if (s == "PLT")  return Kind::Plt;
    if (s == "RCP")  return Kind::Rcp;
    if (s == "EXP")  return Kind::Exp;
    return Kind::Unknown;
}

const char* kindDescription (Kind k) noexcept
{
    switch (k)
    {
        case Kind::Spc:  return "GROWN SPECIMEN";
        case Kind::Clip: return "ARRANGEMENT CAPTURE";
        case Kind::Rec:  return "MASTER RECORDING";
        case Kind::Acq:  return "ACQUIRED AUDIO";
        case Kind::Scn:  return "CAPTURED IMAGE";
        case Kind::Plt:  return "RENDERED PLATE";
        case Kind::Rcp:  return "RECIPE";
        case Kind::Exp:  return "EXPORT";
        default:         return "UNCLASSIFIED";
    }
}

/* ==========================================================================
 *  CLEARANCE
 * ========================================================================== */
const char* clearanceTag (Clearance c) noexcept
{
    switch (c)
    {
        case Clearance::Cleared:      return "CLEARED";
        case Clearance::Review:       return "REVIEW";
        case Clearance::PersonalOnly: return "PERSONAL_ONLY";
        default:                      return "UNREVIEWED";
    }
}

Clearance clearanceFromTag (juce::StringRef t) noexcept
{
    const juce::String s = juce::String (t).trim().toUpperCase();
    if (s == "CLEARED")       return Clearance::Cleared;
    if (s == "REVIEW")        return Clearance::Review;
    if (s == "PERSONAL_ONLY") return Clearance::PersonalOnly;
    return Clearance::Unreviewed;
}

/* --------------------------------------------------------------------------
 * A line-for-line port of CLEARANCE_RULES in tools/exhume.py. The collection
 * keys, the states and the notes are copied verbatim; if that table changes,
 * change this one in the same commit or the CLI and the panel will disagree
 * about whether the same archive.org item can be released.
 *
 * These are not guesses. They encode what is actually known about the
 * collections this project pulls from -- most importantly that the Great 78
 * Project (georgeblood / 78rpm) is offered for research, teaching and private
 * study ONLY, which is a thing you can happily study and must never ship.
 * -------------------------------------------------------------------------- */
namespace
{
struct ClearanceRule { const char* collection; Clearance state; const char* note; };

const ClearanceRule kClearanceRules[] =
{
    { "fedflix",             Clearance::Cleared,
      "US federal works are uncopyrightable in the US" },
    { "librivoxaudio",       Clearance::Cleared,
      "LibriVox volunteers dedicate recordings to the public domain" },
    { "netlabels",           Clearance::Review,
      "CC-licensed netlabel release; check the specific licence" },
    { "prelinger",           Clearance::Review,
      "Collection convention is PD but many items carry no licenseurl" },
    { "prelingerhomemovies", Clearance::Review,
      "Murkier status than the main Prelinger collection" },
    { "georgeblood",         Clearance::PersonalOnly,
      "Great 78 Project: research, teaching and private study ONLY" },
    { "78rpm",               Clearance::PersonalOnly,
      "Great 78 Project: research, teaching and private study ONLY" },
    { "audio_religion",      Clearance::Review,
      "Many items are modern congregational recordings, not PD" },
    { "shortwave-airchecks", Clearance::Review,
      "Off-air recordings of third-party broadcasts" },
    { "dlarc",               Clearance::Review,
      "Digital Library of Amateur Radio; per-item terms vary" },
};

/* exhume.py's rank, exactly: {"PERSONAL_ONLY": 0, "REVIEW": 1, "CLEARED": 2,
 * "UNREVIEWED": 3}, most restrictive (lowest) wins, starting from UNREVIEWED.
 * UNREVIEWED ranks last here because in THIS function it is the "no rule
 * matched" sentinel -- see moreRestrictive() below, where it means something
 * quite different and therefore ranks differently. */
int ruleRank (Clearance c) noexcept
{
    switch (c)
    {
        case Clearance::PersonalOnly: return 0;
        case Clearance::Review:       return 1;
        case Clearance::Cleared:      return 2;
        default:                      return 3;
    }
}
} // namespace

Clearance clearanceForCollections (const juce::StringArray& collections,
                                   juce::String* noteOut)
{
    Clearance best = Clearance::Unreviewed;
    juce::String bestNote ("No rule for this collection; verify manually");

    for (const auto& raw : collections)
    {
        const juce::String c = raw.trim().toLowerCase();
        for (const auto& rule : kClearanceRules)
            if (c == rule.collection && ruleRank (rule.state) < ruleRank (best))
            {
                best = rule.state;
                bestNote = rule.note;
            }
    }

    if (noteOut != nullptr) *noteOut = bestNote;
    return best;
}

Clearance moreRestrictive (Clearance a, Clearance b) noexcept
{
    /* PERSONAL_ONLY > REVIEW > UNREVIEWED > CLEARED. See the long note in
     * Ledger.h: for PROPAGATION down a provenance chain, an ancestor nobody
     * has looked at is worse than one that has been cleared, because the
     * question has not been answered rather than answered yes. */
    auto rank = [] (Clearance c) noexcept -> int
    {
        switch (c)
        {
            case Clearance::PersonalOnly: return 0;
            case Clearance::Review:       return 1;
            case Clearance::Unreviewed:   return 2;
            default:                      return 3;   // Cleared
        }
    };
    return rank (a) <= rank (b) ? a : b;
}

/* ==========================================================================
 *  LINE CODEC
 *
 *  A record is one line. Within the line, fields are TAB-separated and each
 *  field is `key SPACE value` -- session.conf's grammar, packed sideways.
 *  Only three characters can break that shape, so only three are escaped, and
 *  the escape character itself makes four.
 * ========================================================================== */
namespace
{
constexpr char kFieldSep = '\t';

/* Empty values are written as "-" rather than omitted, so that every record
 * carries the same field set and a column-oriented eye (or an awk one-liner)
 * can rely on it. A value that is LITERALLY "-" is escaped to "\-" so the two
 * cases never collide -- an archive.org title of "-" is absurd but the codec
 * should not be the thing that decides that. */
juce::String esc (const juce::String& raw)
{
    if (raw.isEmpty()) return "-";

    juce::String s;
    s.preallocateBytes ((size_t) raw.getNumBytesAsUTF8() + 8);

    for (auto p = raw.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const juce::juce_wchar c = *p;
        switch (c)
        {
            case '\\': s += "\\\\"; break;
            case '\t': s += "\\t";  break;
            case '\n': s += "\\n";  break;
            case '\r': s += "\\r";  break;
            default:   s += juce::String::charToString (c); break;
        }
    }

    if (s == "-") s = "\\-";
    return s;
}

/* Decoding must be a single pass. The obvious chain of replace() calls is
 * wrong: a value containing a literal backslash followed by a 't' encodes as
 * "\\t", and un-escaping "\\" first and then "\t" turns it into a tab. */
juce::String unesc (const juce::String& enc)
{
    if (enc.isEmpty() || enc == "-") return {};

    juce::String out;
    out.preallocateBytes ((size_t) enc.getNumBytesAsUTF8());

    for (auto p = enc.getCharPointer(); ! p.isEmpty(); ++p)
    {
        juce::juce_wchar c = *p;
        if (c == '\\')
        {
            const juce::juce_wchar n = *(p + 1);
            if (n == 0) break;                       // trailing lone backslash
            ++p;
            switch (n)
            {
                case 't':  c = '\t'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case '\\': c = '\\'; break;
                default:   c = n;    break;          // "\-" and anything else
            }
        }
        out += juce::String::charToString (c);
    }
    return out;
}

void put (juce::StringArray& fields, const char* key, const juce::String& value)
{
    fields.add (juce::String (key) + " " + esc (value));
}

/* The keys this build owns. An `extra` entry may not use one of them, because
 * on the next read it would be parsed back into the corresponding member and
 * the caller's value would appear to have overwritten a field it never touched.
 * Anything else -- a caller's "x-seed", or a key written by a future build this
 * one has never heard of -- is written back out EXACTLY as it came in, so the
 * file round-trips through an old binary without losing information. */
bool isReservedKey (const juce::String& k)
{
    static const char* const reserved[] =
    {
        "serial", "utc", "kind", "origin", "derived_from", "sha256", "file",
        "creator", "date", "source", "source_id", "licence", "license",
        "declared_by", "clearance", "note", "tool", "version"
    };
    for (auto* r : reserved)
        if (k == r) return true;
    return false;
}

/* A serial is legible if it can be typed off a printed tag without ambiguity
 * and can never be confused with a field separator. Deliberately permissive
 * about SHAPE, so that the register can also accession the names this project
 * already minted before it existed -- "SPC-4F2A", "CLIP-A-0001" -- rather than
 * refusing to record the very files whose identity is the problem. */
bool serialLooksValid (const juce::String& s)
{
    if (s.length() < 3 || s.length() > 64) return false;
    for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
    {
        /* Compared as a plain int against ASCII literals on purpose:
         * juce_wchar is an unsigned 32-bit type on Windows, and mixing it with
         * char literals in a chain of relational operators is exactly the kind
         * of signed/unsigned comparison that /W4 shouts about and that a
         * reader has to stop and think through. Anything outside ASCII fails
         * the range tests and is rejected, which is the intent. */
        const int c = (int) *p;
        const bool ok = (c >= '0' && c <= '9')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= 'a' && c <= 'z')
                     ||  c == '-' || c == '_' || c == '.';
        if (! ok) return false;
    }
    return true;
}
} // namespace

bool Record::isValid() const
{
    /* Two conditions, and the second is the one that matters. A record without
     * an origin is a record that will one day be shown to a player as a bare
     * serial with no name attached, which is precisely the failure the LOCKER
     * already has. Refuse it at the door. */
    return serialLooksValid (serial) && origin.trim().isNotEmpty();
}

juce::String Record::toLine() const
{
    juce::StringArray f;

    /* `serial` is first, always, so that the parser can identify a record line
     * with a startsWith() and so that `sort` on this file orders by kind and
     * then by accession date without any options. */
    put (f, "serial",       serial);
    put (f, "utc",          utc);
    put (f, "kind",         kindTag (kind));
    put (f, "origin",       origin);
    put (f, "derived_from", derivedFrom.joinIntoString (" "));
    put (f, "sha256",       sha256);
    put (f, "file",         file);
    put (f, "creator",      creator);
    put (f, "date",         date);
    put (f, "source",       source);
    put (f, "source_id",    sourceId);
    put (f, "licence",      licence);
    put (f, "declared_by",  declaredBy);
    put (f, "clearance",    clearanceTag (clearance));
    put (f, "note",         note);
    put (f, "tool",         tool);

    /* Extras last, sorted, so that two runs producing the same record produce
     * byte-identical lines. That is what makes the file diffable in the strong
     * sense and what will let the cross-machine sync detect a genuine
     * divergence rather than a key-ordering artefact. */
    juce::StringArray keys = extra.getAllKeys();
    keys.sort (false);
    for (const auto& k : keys)
    {
        const juce::String key = k.trim();

        /* A key containing a space or a tab cannot be expressed in this
         * grammar at all, and a reserved key would silently overwrite a real
         * field on the next read. Both are dropped rather than mangled --
         * writing a corrupted approximation of somebody's metadata is worse
         * than writing none of it. */
        if (key.isEmpty()
            || key.containsChar (' ')
            || key.containsChar (kFieldSep)
            || isReservedKey (key))
            continue;

        f.add (key + " " + esc (extra[k]));
    }

    return f.joinIntoString ("\t");
}

Record Record::fromLine (const juce::String& line)
{
    Record r;

    juce::StringArray fields;
    fields.addTokens (line, "\t", "");

    for (const auto& field : fields)
    {
        const int sp = field.indexOfChar (' ');
        if (sp <= 0) continue;                          // no key, or empty key

        const juce::String key = field.substring (0, sp);
        const juce::String val = unesc (field.substring (sp + 1));

        if      (key == "serial")       r.serial     = val;
        else if (key == "utc")          r.utc        = val;
        else if (key == "kind")         r.kind       = kindFromTag (val);
        else if (key == "origin")       r.origin     = val;
        else if (key == "derived_from") r.derivedFrom.addTokens (val, " ", "");
        else if (key == "sha256")       r.sha256     = val;
        else if (key == "file")         r.file       = val;
        else if (key == "creator")      r.creator    = val;
        else if (key == "date")         r.date       = val;
        else if (key == "source")       r.source     = val;
        else if (key == "source_id")    r.sourceId   = val;
        else if (key == "licence")      r.licence    = val;
        else if (key == "license")      r.licence    = val;   // tolerate the US spelling
        else if (key == "declared_by")  r.declaredBy = val;
        else if (key == "clearance")    r.clearance  = clearanceFromTag (val);
        else if (key == "note")         r.note       = val;
        else if (key == "tool")         r.tool       = val;
        else
        {
            /* Anything this build does not know about is preserved verbatim,
             * so that an older MORGUE reading a newer ledger does not silently
             * destroy fields it cannot interpret. Nothing here rewrites lines,
             * but a future merge tool will, and it will use this. */
            r.extra.set (key, val);
        }
    }

    r.derivedFrom.removeEmptyStrings();
    r.derivedFrom.removeDuplicates (false);
    return r;
}

/* ==========================================================================
 *  TIME
 * ========================================================================== */
juce::String Ledger::isoUtcNow()
{
    /* juce::Time's field accessors (getYear, getHours, ...) all report LOCAL
     * time, and juce::Time::formatted goes through strftime with localtime.
     * There is no UTC accessor. Shifting the instant backwards by the local
     * UTC offset and then reading the local fields off THAT prints UTC, which
     * is the standard trick and the only one available here.
     *
     * UTC is not a detail. This ledger is designed to be merged across
     * machines; two records timestamped in two different local zones cannot be
     * ordered against each other, and a laptop that crosses a DST boundary
     * would appear to accession material an hour before it acquired it. */
    const juce::Time now = juce::Time::getCurrentTime();
    const juce::Time u (now.toMilliseconds()
                        - (juce::int64) now.getUTCOffsetSeconds() * 1000);

    return juce::String::formatted ("%04d-%02d-%02dT%02d:%02d:%02dZ",
                                    u.getYear(), u.getMonth() + 1, u.getDayOfMonth(),
                                    u.getHours(), u.getMinutes(), u.getSeconds());
}

namespace
{
juce::String utcDatePart()                       // YYMMDD, same clock as above
{
    const juce::Time now = juce::Time::getCurrentTime();
    const juce::Time u (now.toMilliseconds()
                        - (juce::int64) now.getUTCOffsetSeconds() * 1000);
    return juce::String::formatted ("%02d%02d%02d",
                                    u.getYear() % 100, u.getMonth() + 1,
                                    u.getDayOfMonth());
}

/* ==========================================================================
 *  ENTROPY AND THE BOUND THIS FILE CHOSE
 *
 *  Serial: <KIND>-<YYMMDD>-<8 chars of Crockford base32> = 40 bits of entropy.
 *
 *  Crockford's alphabet omits I, L, O and U, so a serial read off a printed
 *  evidence tag or a photograph of one cannot be mistyped as a different valid
 *  serial -- which is the entire point of putting a serial on a tag.
 *
 *  WHY 40 BITS. The namespace a collision has to occur within is one KIND on
 *  one DAY, because the tag and the date are part of the serial. By the
 *  birthday bound the probability that N serials minted into one such
 *  namespace are all distinct is about exp(-N^2 / 2^41), so:
 *
 *      N =     1 000    P(collision) ~ 4.5e-7
 *      N =    10 000    P(collision) ~ 4.5e-5
 *      N = 1 000 000    P(collision) ~ 0.37
 *
 *  Ten thousand accessions of one kind in a single day is already an absurd
 *  figure for a hand-played instrument, and at that volume the odds are one in
 *  twenty-two thousand. Compare the thing this replaces: engine.c's 16-bit
 *  name reaches even odds at 302 specimens -- an afternoon.
 *
 *  40 bits was also chosen for the shape it makes. The serial is 19 characters
 *  (ACQ-260805-K7J4QWMR), which fits an evidence tag, fits the LOCKER column,
 *  and reads as three groups. 64 bits would have cost 13 characters of suffix
 *  for a bound nobody will ever reach. And because appends are checked against
 *  the loaded register before they are written (see mint()), a local collision
 *  is impossible rather than merely improbable; the bound above governs only
 *  the case that two machines mint the same serial while disconnected, which
 *  is the one case the check cannot cover.
 * ========================================================================== */
constexpr char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford

juce::uint64 entropy64()
{
    /* thread_local, so no lock and no shared state; seeded from
     * std::random_device (a real CSPRNG on both toolchains this ships to),
     * mixed with the high-resolution clock and a process-wide counter so that
     * two threads which somehow seed identically still diverge immediately.
     *
     * juce::Random::getSystemRandom() is deliberately NOT used: it is a single
     * shared object and is not thread-safe, and serials get minted from
     * whichever worker happens to have finished a download. */
    static std::atomic<juce::uint64> salt { 0 };

    thread_local std::mt19937_64 rng = []
    {
        std::random_device rd;
        juce::uint64 s = ((juce::uint64) rd() << 32) ^ (juce::uint64) rd();
        s ^= (juce::uint64) juce::Time::getHighResolutionTicks();
        s ^= (juce::uint64) (juce::pointer_sized_uint) &salt;
        return std::mt19937_64 (s);
    }();

    /* The golden-ratio odd constant is the usual choice for a counter stride:
     * it visits every 64-bit value before repeating and decorrelates
     * successive draws. */
    return rng() ^ salt.fetch_add (0x9E3779B97F4A7C15ull);
}

juce::String base32Suffix()
{
    const juce::uint64 e = entropy64();
    char out[9];
    for (int i = 0; i < 8; ++i)
        out[i] = kBase32[(int) ((e >> (5 * i)) & 31u)];   // 8 * 5 = 40 bits
    out[8] = 0;
    return juce::String (juce::CharPointer_ASCII (out));
}
} // namespace

/* ==========================================================================
 *  LEDGER -- lifetime and paths
 * ========================================================================== */
Ledger& Ledger::shared()
{
    /* Function-local static: constructed on first use, which is guaranteed to
     * be after Main.cpp has planted the engine's root, so file() can ask
     * Session.h for a real path instead of the XDG-style relative fallback
     * bb_config_path() invents when it is asked too early. */
    static Ledger instance;
    return instance;
}

void Ledger::bootstrap()
{
    shared().loadAsync();
}

juce::File Ledger::file() const
{
    /* Resolved once and remembered. The session root cannot change while the
     * console is running -- bb_config_set_root() is called exactly once, from
     * Main.cpp, before anything else -- and caching it means the append path
     * does no engine calls and no string building under the write lock.
     *
     * Under dataLock because file() is reachable from several worker threads
     * at once and juce::String's copy-on-write reference count is not a
     * substitute for a mutex during ASSIGNMENT. */
    const juce::ScopedLock sl (dataLock);

    if (cachedPath.isEmpty())
        cachedPath = morgue::morgueDir().getChildFile ("ACCESSION.ledger")
                                        .getFullPathName();
    return juce::File (cachedPath);
}

juce::String Ledger::relativeToRoot (const juce::File& f)
{
    if (f == juce::File()) return {};

    const juce::File root = morgue::morgueDir();
    if (f.isAChildOf (root))
    {
        /* Stored with forward slashes on every platform. A ledger written on
         * Windows and synced to the MacBook -- which is the entire point of
         * the analog round trip -- must still resolve its files, and
         * "MORGUE\\ACQ\\x.wav" does not resolve anywhere but Windows.
         * juce::File normalises "/" back to the native separator on the way
         * in, so resolve() below needs no matching special case. */
        return f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
    }

    /* Outside the session root there is nothing portable to store, so store
     * the truth and let resolve() fail honestly on another machine. */
    return f.getFullPathName();
}

juce::File Ledger::resolve (const Record& r) const
{
    if (r.file.isEmpty()) return {};
    if (juce::File::isAbsolutePath (r.file)) return juce::File (r.file);
    return morgue::morgueDir().getChildFile (r.file);
}

juce::String Ledger::sha256OfFile (const juce::File& f)
{
    if (! f.existsAsFile()) return {};

    /* juce::SHA256's File constructor streams the file rather than loading it,
     * so a 400 MB archive.org original does not become 400 MB of resident
     * memory. This still takes seconds on a large file: WORKER THREADS ONLY. */
    return juce::SHA256 (f).toHexString();
}

/* ==========================================================================
 *  MINT
 * ========================================================================== */
juce::String Ledger::mint (Kind k) const
{
    const juce::String prefix = juce::String (kindTag (k)) + "-" + utcDatePart() + "-";

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const juce::String candidate = prefix + base32Suffix();
        {
            const juce::ScopedLock sl (dataLock);
            if (! bySerial.contains (candidate))
                return candidate;
        }
    }

    /* Eight consecutive 40-bit collisions against the loaded register does not
     * happen; if it somehow does, the random source is broken and the honest
     * response is to stop trusting it for uniqueness rather than to loop
     * forever. Widen the suffix and take the result -- a 19-character serial
     * is a convenience, uniqueness is not. */
    return prefix + base32Suffix() + base32Suffix();
}

/* ==========================================================================
 *  WRITE
 * ========================================================================== */
namespace
{
juce::String ledgerHeader()
{
    /* Modelled on the header bb_config_save() writes at engine.c:2063, down to
     * the invitation to edit it -- the same file, in the same voice. */
    return "# MORGUE ACCESSION register -- plain text, one record per line.\n"
           "# Same `key value` grammar as session.conf; fields are TAB-separated.\n"
           "# Append-only: MORGUE never rewrites or reorders a line. You may\n"
           "# correct a field by hand; do not delete lines, and do not renumber.\n"
           "version 1";
}
} // namespace

bool Ledger::writeLine (const juce::String& line)
{
    const juce::File f = file();
    f.getParentDirectory().createDirectory();

    /* A cross-process lock, and it is not decoration. juce::FileOutputStream
     * does NOT open with O_APPEND -- it seeks to the end of the file at
     * construction -- so two MORGUE instances (or MORGUE and a future sync
     * daemon) writing at the same moment would both seek to the same offset
     * and one line would overwrite the other. That is real corruption, not a
     * reordering, so if the lock cannot be taken we refuse the write and let
     * the caller queue the record rather than risk it.
     *
     * The lock is an OS primitive (a named mutex on Windows, fcntl on POSIX),
     * both of which the kernel releases when the holder dies -- so a crashed
     * MORGUE cannot leave a stale lock that wedges the next run. */
    juce::InterProcessLock ipc ("MORGUE.ACCESSION");
    if (! ipc.enter (5000))
        return false;

    bool ok = false;
    {
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            if (out.getPosition() == 0)
                out.writeText (ledgerHeader() + "\n", false, false, "\n");

            /* The record and its terminating newline go out as ONE write into
             * one buffer, then one flush. That is what makes the append
             * crash-safe: the only thing a power failure can damage is the
             * tail of the final line, and loadNow() detects and discards a
             * final line that is not newline-terminated. An earlier record can
             * never be touched, because nothing ever seeks backwards in this
             * file. */
            out.writeText (line + "\n", false, false, "\n");

            /* Explicit line endings of "\n" on every platform. Left to
             * itself this would be LF anyway, but being explicit means a
             * ledger written on Windows and one written on the MacBook are
             * byte-comparable, which matters the day they are merged. */

            out.flush();                    // flushes the buffer AND fsyncs
            ok = out.getStatus().wasOk();
        }
    }

    ipc.exit();
    return ok;
}

void Ledger::indexLocked (int recordIndex)
{
    const Record& r = records[(size_t) recordIndex];
    if (r.serial.isNotEmpty()) bySerial.set (r.serial, recordIndex);
    if (r.sha256.isNotEmpty()) byHash.set (r.sha256, recordIndex);
}

bool Ledger::append (Record& r)
{
    /* Serialise the whole append. Held only by workers; see the lock note in
     * Ledger.h. */
    const juce::ScopedLock writing (writeLock);

    if (r.utc.isEmpty())    r.utc = isoUtcNow();
    if (r.serial.isEmpty()) r.serial = mint (r.kind);

    if (! r.isValid())
    {
        /* The one hard refusal in this file. A record with no origin would
         * become a row in the LOCKER showing a serial and nothing else, which
         * is the defect this whole file exists to remove; writing it would be
         * worse than not writing it, because it would look like provenance. */
        jassertfalse;
        return false;
    }

    const juce::String line = r.toLine();
    const bool wrote = writeLine (line);

    {
        const juce::ScopedLock sl (dataLock);

        /* In memory either way. A caller that appends and then immediately
         * looks up its own record must find it even if the disk is full --
         * the session should degrade to "your provenance is not durable yet",
         * not to "your provenance does not exist". */
        records.push_back (r);
        indexLocked ((int) records.size() - 1);

        if (! wrote)
            pending.push_back (line);
    }

    return wrote;
}

void Ledger::appendAsync (Record r, std::function<void (bool, const Record&)> done)
{
    /* The shape Chrome.cpp::growSpecimen() established: launch, work, hop back
     * to the message thread. The completion's captures are the CALLER's
     * problem -- capture a juce::Component::SafePointer if a panel is going to
     * touch itself in there. */
    juce::Thread::launch ([r, done]() mutable
    {
        const bool ok = Ledger::shared().append (r);

        if (done)
            juce::MessageManager::callAsync ([done, ok, r] { done (ok, r); });
    });
}

Record Ledger::adopt (const juce::File& source, Kind kind, const juce::String& origin,
                      bool renameToSerial, Record proto)
{
    Record r = std::move (proto);
    r.kind   = kind;
    r.origin = origin;

    if (r.utc.isEmpty())    r.utc    = isoUtcNow();
    if (r.serial.isEmpty()) r.serial = mint (kind);

    juce::File onDisk = source;

    if (renameToSerial && source.existsAsFile())
    {
        /* This is the move that actually retires engine.c's colliding
         * "SPC-%04X.wav". Once the artefact is named after its serial, two
         * specimens grown from seeds that share their low sixteen bits are two
         * files, not one file written twice -- and the name on disk, the name
         * in the ledger and the name in the LOCKER are the same string.
         *
         * The extension is kept: the LOCKER, GRAIN MASS and every file chooser
         * in the console decide what a file IS from its extension. */
        const juce::String ext = source.getFileExtension();
        juce::File target = source.getParentDirectory()
                                  .getChildFile (r.serial + ext);

        /* Refusing to clobber is not paranoia -- it is the entire lesson of
         * the defect above. If the target somehow exists, keep both. */
        for (int n = 2; target.existsAsFile() && n <= 99; ++n)
            target = source.getParentDirectory()
                           .getChildFile (r.serial + "-" + juce::String (n) + ext);

        if (! target.existsAsFile() && source.moveFileTo (target))
            onDisk = target;
    }

    if (onDisk.existsAsFile())
    {
        if (r.sha256.isEmpty()) r.sha256 = sha256OfFile (onDisk);
        if (r.file.isEmpty())   r.file   = relativeToRoot (onDisk);
    }

    /* The return value is deliberately dropped. The file is adopted and renamed
     * regardless -- it is on disk and it is named after its serial. What can
     * fail here is durability of the RECORD, and append() has already put it in
     * memory and queued the line; the caller learns about that from
     * pendingCount(). Returning the completed record rather than an empty one
     * keeps the caller's own bookkeeping -- a panel that just placed a clip and
     * needs its serial -- intact either way. */
    (void) append (r);

    return r;
}

int Ledger::pendingCount() const
{
    const juce::ScopedLock sl (dataLock);
    return (int) pending.size();
}

bool Ledger::flushPending()
{
    const juce::ScopedLock writing (writeLock);

    std::vector<juce::String> todo;
    {
        const juce::ScopedLock sl (dataLock);
        todo = pending;
    }
    if (todo.empty()) return true;

    std::vector<juce::String> stillPending;
    for (const auto& line : todo)
        if (! writeLine (line))
            stillPending.push_back (line);

    {
        const juce::ScopedLock sl (dataLock);
        pending = stillPending;
    }

    return stillPending.empty();
}

/* ==========================================================================
 *  LOAD
 * ========================================================================== */
bool Ledger::loadNow()
{
    const juce::File f = file();

    /* Everything up to the swap happens in LOCAL containers, off the lock.
     * Parsing a large ledger is milliseconds of string work; doing it while
     * holding a lock a paint routine can want is how a file on disk turns into
     * a dropped frame. */
    std::vector<Record> parsed;
    juce::HashMap<juce::String, int> serialIndex;
    juce::HashMap<juce::String, int> hashIndex;

    if (f.existsAsFile())
    {
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return false;

        parsed.reserve (1024);

        while (! in.isExhausted())
        {
            const juce::String line = in.readNextLine();

            if (line.isEmpty()) continue;
            if (line.startsWithChar ('#')) continue;

            /* Every record line begins with the serial field. Anything else --
             * the `version` line, a note somebody typed in, a future line type
             * this build has never heard of -- is skipped rather than
             * mis-parsed. */
            if (! line.startsWith ("serial ")) continue;

            Record r = Record::fromLine (line);
            if (r.serial.isNotEmpty())
                parsed.push_back (std::move (r));
        }

        /* The torn-tail check. An append-only file damaged by a crash or a
         * power cut can only be damaged in one place: the end of the last
         * line, because nothing ever seeks backwards in it. If the file does
         * not end in a newline, the final line was never completely written,
         * and a half-written record is exactly the kind of thing that would
         * look plausible (it would still start with a valid serial) while
         * carrying a truncated origin or a truncated hash. Drop it.
         *
         * This is also why the format needs no checksums: append-only plus a
         * newline terminator gives the same guarantee for six bytes a record
         * instead of ten, and stays readable. */
        const juce::int64 sz = f.getSize();
        if (sz > 0 && ! parsed.empty())
        {
            juce::FileInputStream tail (f);
            if (tail.openedOk() && tail.setPosition (sz - 1))
            {
                char last = 0;
                if (tail.read (&last, 1) == 1 && last != '\n' && last != '\r')
                    parsed.pop_back();
            }
        }
    }

    for (size_t i = 0; i < parsed.size(); ++i)
    {
        if (parsed[i].serial.isNotEmpty()) serialIndex.set (parsed[i].serial, (int) i);
        if (parsed[i].sha256.isNotEmpty()) hashIndex  .set (parsed[i].sha256, (int) i);
    }

    {
        const juce::ScopedLock sl (dataLock);

        /* Three swaps and a bool. This is the only moment the message thread
         * can be made to wait for the load, and it is O(1). */
        records.swap (parsed);
        bySerial.swapWith (serialIndex);
        byHash.swapWith (hashIndex);
        loaded = true;
    }

    return true;
}

void Ledger::loadAsync (std::function<void (int)> done)
{
    juce::Thread::launch ([done]
    {
        Ledger::shared().loadNow();
        const int n = Ledger::shared().size();

        if (done)
            juce::MessageManager::callAsync ([done, n] { done (n); });
    });
}

bool Ledger::isLoaded() const
{
    const juce::ScopedLock sl (dataLock);
    return loaded;
}

int Ledger::size() const
{
    const juce::ScopedLock sl (dataLock);
    return (int) records.size();
}

/* ==========================================================================
 *  READ
 * ========================================================================== */
bool Ledger::lookup (const juce::String& serial, Record& out) const
{
    if (serial.isEmpty()) return false;

    const juce::ScopedLock sl (dataLock);
    if (! bySerial.contains (serial)) return false;

    out = records[(size_t) bySerial[serial]];
    return true;
}

bool Ledger::findBySha256 (const juce::String& hash, Record& out) const
{
    if (hash.isEmpty()) return false;

    const juce::ScopedLock sl (dataLock);
    if (! byHash.contains (hash)) return false;

    out = records[(size_t) byHash[hash]];
    return true;
}

bool Ledger::findByFile (const juce::File& f, Record& out) const
{
    if (f == juce::File()) return false;

    const juce::String rel  = relativeToRoot (f);
    const juce::String name = f.getFileName();

    const juce::ScopedLock sl (dataLock);

    /* Exact stored path first. */
    for (const auto& r : records)
        if (r.file.isNotEmpty() && r.file == rel)
        {
            out = r;
            return true;
        }

    /* Then the basename, because files get moved around inside the session
     * root by hand and a record whose path went stale is still the right
     * record. Newest-first, so that if a name really was reused the most
     * recent accession wins -- which is what the LOCKER's newest-first sort
     * would show anyway. */
    for (auto it = records.rbegin(); it != records.rend(); ++it)
        if (it->file.isNotEmpty()
            && it->file.fromLastOccurrenceOf ("/", false, false) == name)
        {
            out = *it;
            return true;
        }

    return false;
}

std::vector<Record> Ledger::ancestry (const juce::String& serial) const
{
    std::vector<Record> out;
    if (serial.isEmpty()) return out;

    /* Breadth-first, so a record with two parents lists both before either
     * grandparent -- which is the order a human reads a provenance chain in,
     * and the order that puts the nearest (most likely to be wrong) edge
     * first.
     *
     * The visited set and the depth cap are not theoretical. This file is
     * documented as hand-editable, so sooner or later somebody will paste a
     * serial into its own derived_from, and an unguarded walk would hang the
     * message thread forever inside a paint routine. */
    juce::StringArray visited;
    std::vector<juce::String> frontier { serial };
    visited.add (serial);

    const juce::ScopedLock sl (dataLock);

    for (int depth = 0; depth < 64 && ! frontier.empty(); ++depth)
    {
        std::vector<juce::String> next;

        for (const auto& s : frontier)
        {
            if (! bySerial.contains (s)) continue;
            const Record& r = records[(size_t) bySerial[s]];

            for (const auto& parent : r.derivedFrom)
            {
                if (parent.isEmpty() || visited.contains (parent)) continue;
                visited.add (parent);

                if (bySerial.contains (parent))
                {
                    out.push_back (records[(size_t) bySerial[parent]]);
                    next.push_back (parent);
                }
                else
                {
                    /* A parent that is named but not present. This is what a
                     * half-synced ledger looks like, and it is far better to
                     * surface it as a stub -- with the serial the player can
                     * go and find -- than to silently prune the branch and
                     * report a shorter, cleaner, wrong chain. */
                    Record stub;
                    stub.serial = parent;
                    stub.origin = "(not in this register)";
                    stub.clearance = Clearance::Unreviewed;
                    stub.note = "Parent serial named by a descendant but not "
                                "present here. Sync the other machine's ledger.";
                    out.push_back (stub);
                }
            }
        }

        frontier.swap (next);
    }

    return out;
}

std::vector<Record> Ledger::children (const juce::String& serial) const
{
    std::vector<Record> out;
    if (serial.isEmpty()) return out;

    const juce::ScopedLock sl (dataLock);
    for (const auto& r : records)
        if (r.derivedFrom.contains (serial))
            out.push_back (r);

    return out;
}

std::vector<Record> Ledger::listByKind (Kind k) const
{
    std::vector<Record> out;

    const juce::ScopedLock sl (dataLock);
    for (const auto& r : records)
        if (r.kind == k)
            out.push_back (r);

    /* Newest first, matching the LOCKER's own sort (Chrome.cpp's
     * LockerFileCmp). Serials begin with the kind and then the accession date,
     * so a plain reverse string sort within one kind IS chronological -- which
     * is one of the reasons the date is in the serial at all. */
    std::sort (out.begin(), out.end(), [] (const Record& a, const Record& b)
    {
        if (a.utc != b.utc) return a.utc > b.utc;
        return a.serial > b.serial;
    });

    return out;
}

std::vector<Record> Ledger::all() const
{
    const juce::ScopedLock sl (dataLock);
    return records;
}

Clearance Ledger::effectiveClearance (const juce::String& serial) const
{
    Record self;
    if (! lookup (serial, self))
        return Clearance::Unreviewed;

    Clearance c = self.clearance;
    for (const auto& parent : ancestry (serial))
        c = moreRestrictive (c, parent.clearance);

    return c;
}

/* ==========================================================================
 *  CREDITS -- the artefact that makes the discipline pay off
 * ========================================================================== */
namespace
{
/* A reachable record earns a line in the credits sheet if there is somebody to
 * credit or something to resolve. MORGUE's own intermediate work -- a specimen
 * grown from an expression, a clip captured off the arrangement -- has no
 * external creator, no source URL and no licence, and listing it would bury the
 * three entries that actually matter under forty that do not. But anything
 * whose clearance is REVIEW or PERSONAL_ONLY stays in regardless of how bare it
 * is, because those are the entries a release has to act on. */
bool earnsCredit (const Record& r)
{
    if (r.creator.isNotEmpty() || r.source.isNotEmpty()
        || r.licence.isNotEmpty() || r.sourceId.isNotEmpty())
        return true;

    return r.clearance == Clearance::Review || r.clearance == Clearance::PersonalOnly;
}
} // namespace

std::vector<Credit> Ledger::credits (const juce::StringArray& shippedSerials) const
{
    /* serial -> (credit, greatest distance from any shipped item). The
     * distance is what orders the sheet: the acquisition four steps back gets
     * the line, and it gets it before the render that used it. */
    std::vector<Credit> out;
    juce::StringArray seen;
    std::vector<int> depths;

    auto emit = [&] (const Record& r, int depth, const juce::String& usedBy)
    {
        if (! earnsCredit (r)) return;

        const int existing = seen.indexOf (r.serial);
        if (existing >= 0)
        {
            depths[(size_t) existing] = std::max (depths[(size_t) existing], depth);
            out[(size_t) existing].usedBy.addIfNotAlreadyThere (usedBy);
            return;
        }

        Credit c;
        c.serial     = r.serial;
        c.origin     = r.origin;
        c.creator    = r.creator;
        c.date       = r.date;
        c.source     = r.source;
        c.licence    = r.licence;
        c.declaredBy = r.declaredBy;
        c.clearance  = r.clearance;
        c.note       = r.note;
        c.usedBy.add (usedBy);

        seen.add (r.serial);
        depths.push_back (depth);
        out.push_back (c);
    };

    for (const auto& shipped : shippedSerials)
    {
        Record self;
        if (! lookup (shipped, self)) continue;

        /* The shipped record itself can earn a credit -- an acquisition
         * released as-is has no ancestors and would otherwise produce an empty
         * sheet, which is the single worst possible output of this function. */
        emit (self, 0, shipped);

        int depth = 1;
        for (const auto& parent : ancestry (shipped))
            emit (parent, depth++, shipped);
    }

    /* Roots first: greatest distance from anything shipped, then by serial so
     * that two runs over the same set produce the same sheet. */
    std::vector<size_t> order (out.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort (order.begin(), order.end(), [&] (size_t a, size_t b)
    {
        if (depths[a] != depths[b]) return depths[a] > depths[b];
        return out[a].serial < out[b].serial;
    });

    std::vector<Credit> sorted;
    sorted.reserve (out.size());
    for (auto i : order) sorted.push_back (out[i]);
    return sorted;
}

juce::String Ledger::creditsText (const std::vector<Credit>& list)
{
    juce::StringArray lines;

    /* Deliberately pure ASCII. This block is written to a text file, pasted
     * into a Bandcamp field, mailed to a rights holder and printed on an
     * insert; every one of those is somewhere a stray U+00B7 turns into
     * mojibake in front of somebody whose work is being credited. */
    lines.add ("MORGUE -- ACCESSION REGISTER -- CREDITS");
    lines.add ("Generated " + isoUtcNow()
               + "  --  " + juce::String ((int) list.size()) + " SOURCES");
    lines.add (juce::String());

    if (list.empty())
    {
        lines.add ("No external sources in the provenance of the supplied serials.");
        lines.add ("Either everything shipped is original, or nothing was accessioned.");
        lines.add ("Check the second possibility before believing the first.");
        return lines.joinIntoString ("\n") + "\n";
    }

    for (const auto& c : list)
    {
        lines.add (c.serial + "  "
                   + (c.origin.isNotEmpty() ? c.origin : juce::String ("(unnamed)")));

        auto field = [&lines] (const char* key, const juce::String& v)
        {
            if (v.isNotEmpty())
                lines.add ("    " + juce::String (key).paddedRight (' ', 10) + v);
        };

        field ("creator",  c.creator);
        field ("date",     c.date);
        field ("source",   c.source);

        if (c.licence.isNotEmpty())
            lines.add ("    " + juce::String ("licence").paddedRight (' ', 10)
                       + c.licence
                       + (c.declaredBy.isNotEmpty()
                              ? "   (declared by " + c.declaredBy + ")"
                              : juce::String()));

        lines.add ("    " + juce::String ("clearance").paddedRight (' ', 10)
                   + clearanceTag (c.clearance)
                   + (c.note.isNotEmpty() ? "   " + c.note : juce::String()));

        if (! c.usedBy.isEmpty())
            lines.add ("    " + juce::String ("used by").paddedRight (' ', 10)
                       + c.usedBy.joinIntoString (", "));

        lines.add (juce::String());
    }

    /* The section that stops this being a document that only lists the easy
     * cases. Everything that is not CLEARED is repeated at the bottom, where a
     * human doing a release check will actually read it. */
    juce::StringArray blockers;
    for (const auto& c : list)
        if (c.clearance != Clearance::Cleared)
            blockers.add ("  " + juce::String (clearanceTag (c.clearance)).paddedRight (' ', 15)
                          + c.serial + "  " + c.origin);

    lines.add ("------------------------------------------------------------");
    if (blockers.isEmpty())
    {
        lines.add ("NOT CLEARED: none. Every source above is cleared for release.");
    }
    else
    {
        lines.add ("NOT CLEARED -- resolve or remove before release:");
        lines.add (juce::String());
        lines.addArray (blockers);
        lines.add (juce::String());
        lines.add ("PERSONAL_ONLY means research, teaching and private study only.");
        lines.add ("It is not a warning. It does not ship.");
    }

    return lines.joinIntoString ("\n") + "\n";
}

} // namespace morgue
