/* ExhumePanel.cpp -- see ExhumePanel.h.
 *
 * A faithful port of tools/exhume.py behind a MORGUE panel. The script's
 * endpoint choice, field list, clearance rules, node resolution, path encoding
 * and politeness are all reproduced here; where this file diverges it says so
 * and says why. The script is the reference implementation because it is the
 * one that has been run against the live API.
 */

#include "ExhumePanel.h"

#include "AudioEngine.h"
#include "Session.h"

#include <juce_cryptography/juce_cryptography.h>   // juce::MD5 -- the archive's
                                                   // own per-file checksum is
                                                   // the download verifier
#include <algorithm>
#include <cmath>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* ==========================================================================
 *  Local helpers, constants and the network client.
 * ========================================================================== */
namespace
{

/* This panel's evidence-tag serial. It is NOT added to Theme.h's SerialNo
 * table from here -- that header belongs to the theme, and one panel adding a
 * constant to it is how a shared header turns into a junk drawer. If the
 * console wants it centralised, `EXHUME = "N.72-0427"` goes in SerialNo and
 * this line goes away. */
constexpr const char* kSerial = "N.72-0427";

constexpr int kQueryH   = 32;
constexpr int kChipRowH = 22;
constexpr int kFootH    = 26;
constexpr int kRows     = 40;      // results per page
constexpr int kSegW     = 56;      // AUDIO / MOVIES / ANY
constexpr int kSegGap   = 2;
constexpr int kBtnW     = 74;

int textW (const juce::Font& f, const juce::String& s)
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
}

/* ---- the collection chips ------------------------------------------------
 * EVERY IDENTIFIER BELOW HAS BEEN VERIFIED TO EXIST. That is not a formality:
 * archive.org answers a search for a collection that does not exist with a
 * cheerful HTTP 200 and zero results, which is indistinguishable from a
 * collection that exists and is empty, which is indistinguishable from a query
 * that is simply too narrow. An afternoon disappears into that.
 *
 * THESE PLAUSIBLE-SOUNDING IDENTIFIERS DO NOT EXIST, and are listed here so
 * that nobody adds them back in good faith:
 *
 *      usgovernmentfilms      nationalarchives      ConetProject
 *      conet_project          numbers_stations      shortwave
 *      greatest78             gratefuldead
 *
 * (tools/exhume.py carries the same list as KNOWN_BAD. If you find a real one,
 * add it in both places or the CLI and the panel will disagree.) */
struct CollectionChip { const char* id; const char* label; };

const CollectionChip kCollections[] =
{
    { "",                      "ALL" },
    { "prelinger",             "PRELINGER" },
    { "prelingerhomemovies",   "PRELINGER HOME" },
    { "librivoxaudio",         "LIBRIVOX" },
    { "FedFlix",               "FEDFLIX" },
    { "audio_religion",        "RELIGION" },
    { "shortwave-airchecks",   "SHORTWAVE AIRCHECKS" },
    { "dlarc",                 "DLARC" },
    { "netlabels",             "NETLABELS" },
    { "78rpm",                 "78RPM" },
    { "georgeblood",           "GEORGE BLOOD" },
    { "audio_news",            "AUDIO NEWS" },
    { "newsandpublicaffairs",  "NEWS/PUBLIC AFFAIRS" },
    { "lltns",                 "LLTNS" },
    { "opensource_audio",      "OPEN SOURCE AUDIO" },
};
constexpr int kNumCollections = (int) (sizeof (kCollections) / sizeof (kCollections[0]));

const char* const kMediaNames[3] = { "AUDIO", "MOVIES", "ANY" };

/* ---- politeness ----------------------------------------------------------
 * $MORGUE_CONTACT is read exactly as the script reads it. An unset contact is
 * not fatal -- the panel still works -- but the footer says so, because a
 * client that cannot be contacted when it misbehaves is a client that gets
 * blocked rather than emailed. */
juce::String contactAddress()
{
    const juce::String c = juce::SystemStats::getEnvironmentVariable ("MORGUE_CONTACT", {});
    return c.trim().isNotEmpty() ? c.trim() : juce::String ("unset-contact");
}

juce::String userAgent()
{
    return "MORGUE-EXHUME/0.1 (+noise instrument; " + contactAddress() + ")";
}

juce::String requestHeaders()
{
    return "User-Agent: " + userAgent() + "\r\n";
}

/* The gate. Two requests in flight at most, 500 ms minimum between starts --
 * the script's MIN_INTERVAL, with a concurrency cap added because a panel can
 * have a search, a metadata read and a download all wanting the wire at once
 * where the CLI could only ever have one.
 *
 * Worker threads only. It sleeps. */
class Gate
{
public:
    static bool acquire (const std::atomic<bool>* cancelFlag)
    {
        for (;;)
        {
            {
                const juce::ScopedLock sl (cs());
                const double now = juce::Time::getMillisecondCounterHiRes();

                if (inFlight() < kMaxInFlight && now - lastStart() >= kMinIntervalMs)
                {
                    ++inFlight();
                    lastStart() = now;
                    return true;
                }
            }

            if (cancelFlag != nullptr && cancelFlag->load())
                return false;

            juce::Thread::sleep (25);
        }
    }

    static void release()
    {
        const juce::ScopedLock sl (cs());
        if (inFlight() > 0) --inFlight();
    }

private:
    static constexpr int    kMaxInFlight   = 2;
    static constexpr double kMinIntervalMs = 500.0;

    static juce::CriticalSection& cs()   { static juce::CriticalSection c; return c; }
    static int&    inFlight()            { static int n = 0; return n; }
    static double& lastStart()           { static double t = -1.0e9; return t; }
};

struct GateHold
{
    bool held = false;
    explicit GateHold (const std::atomic<bool>* c) { held = Gate::acquire (c); }
    ~GateHold() { if (held) Gate::release(); }
};

/* Sleep in slices so a cancel is honoured inside a Retry-After wait. A 503
 * from archive.org can carry a Retry-After of 30 s or more, and a player who
 * has pressed CANCEL should not watch a progress line sit there for half a
 * minute out of politeness to a machine. */
bool waitMs (double ms, const std::atomic<bool>* cancelFlag)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        if (cancelFlag != nullptr && cancelFlag->load())
            return false;
        juce::Thread::sleep (50);
    }
    return true;
}

struct NetResult
{
    bool ok = false;
    int  status = 0;
    juce::String body;
    juce::String error;         // already in the console's voice, caps
};

constexpr int kMaxRetries = 4;

/* GET a text resource with backoff. Retry-After honoured on 429 and 503, 5xx
 * retried with exponential backoff, 4xx surfaced immediately (retrying a 404
 * is just rudeness at a slower rate). WORKER THREADS ONLY. */
NetResult getText (const juce::URL& u, const std::atomic<bool>* cancelFlag)
{
    NetResult r;

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (cancelFlag != nullptr && cancelFlag->load())
        {
            r.error = "CANCELLED";
            return r;
        }

        GateHold hold (cancelFlag);
        if (! hold.held) { r.error = "CANCELLED"; return r; }

        int status = 0;
        juce::StringPairArray headers;

        /* numRedirectsToFollow(2) here and 0 on every file transfer. Two is
         * enough for a scheme or host normalisation on the API endpoints and
         * is not enough to end up on a CDN node by accident; the download path
         * below allows none at all, which is fact 2 in the header enforced by
         * the transport rather than by good intentions. */
        std::unique_ptr<juce::InputStream> in (
            u.createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders (requestHeaders())
                    .withConnectionTimeoutMs (30000)
                    .withNumRedirectsToFollow (2)
                    .withStatusCode (&status)
                    .withResponseHeaders (&headers)));

        const bool opened = in != nullptr;
        juce::String body;
        if (opened)
            body = in->readEntireStreamAsString();
        in.reset();

        r.status = status;

        if (! opened)
        {
            /* No stream at all: DNS, no route, TLS, or the machine is on a
             * train. Retry a couple of times -- a laptop waking up produces
             * exactly this -- then say so plainly. */
            if (attempt + 1 >= kMaxRetries)
            {
                r.error = "NO ROUTE TO ARCHIVE.ORG";
                return r;
            }
            if (! waitMs (500.0 * (1 << attempt), cancelFlag)) { r.error = "CANCELLED"; return r; }
            continue;
        }

        if (status == 429 || status == 503)
        {
            const juce::String ra = headers["Retry-After"];
            const double wait = ra.containsOnly ("0123456789") && ra.isNotEmpty()
                                    ? juce::jlimit (1.0, 120.0, (double) ra.getIntValue())
                                    : (double) (1 << attempt);
            if (! waitMs (wait * 1000.0, cancelFlag)) { r.error = "CANCELLED"; return r; }
            continue;
        }

        if (status >= 500)
        {
            if (attempt + 1 >= kMaxRetries)
            {
                r.error = "ARCHIVE.ORG RETURNED " + juce::String (status);
                return r;
            }
            if (! waitMs (1000.0 * (1 << attempt), cancelFlag)) { r.error = "CANCELLED"; return r; }
            continue;
        }

        if (status >= 400)
        {
            r.error = "ARCHIVE.ORG REFUSED THE REQUEST (" + juce::String (status) + ")";
            return r;
        }

        /* Some backends report status 0 for a perfectly good body; trust the
         * body in that case rather than inventing a failure. */
        if (body.isEmpty())
        {
            r.error = "EMPTY REPLY";
            return r;
        }

        r.ok   = true;
        r.body = body;
        return r;
    }

    r.error = "GAVE UP AFTER " + juce::String (kMaxRetries) + " ATTEMPTS";
    return r;
}

/* ---- var helpers ---------------------------------------------------------
 * archive.org returns `creator`, `collection`, `subject` and friends as EITHER
 * a bare string OR an array of strings, per item, with no way to predict which.
 * The script normalises through as_list(); so does this. */
juce::StringArray asList (const juce::var& v)
{
    juce::StringArray out;

    if (auto* arr = v.getArray())
    {
        for (const auto& e : *arr)
        {
            const juce::String s = e.toString().trim();
            if (s.isNotEmpty()) out.add (s);
        }
        return out;
    }

    const juce::String s = v.toString().trim();
    if (s.isNotEmpty()) out.add (s);
    return out;
}

juce::var prop (const juce::var& v, const char* key)
{
    return v.getProperty (juce::Identifier (key), juce::var());
}

juce::String str (const juce::var& v, const char* key)
{
    const juce::StringArray l = asList (prop (v, key));
    return l.isEmpty() ? juce::String() : l.joinIntoString ("; ");
}

/* ---- URLs ---------------------------------------------------------------- */

/* advancedsearch.php, NEVER the scrape API. See fact 1 in the header.
 *
 * `fl[]` goes out percent-encoded as `fl%5B%5D` because JUCE escapes parameter
 * names; PHP decodes it back to `fl[]` before it ever reaches the query
 * parser, so the repeated-field form works exactly as it does from curl. */
juce::URL searchUrl (const juce::String& query, int rows, int page)
{
    static const char* const fields[] =
    {
        "identifier", "title", "creator", "date", "mediatype",
        "collection", "licenseurl", "downloads"
    };

    juce::URL u ("https://archive.org/advancedsearch.php");
    u = u.withParameter ("q", query)
         .withParameter ("rows", juce::String (rows))
         .withParameter ("page", juce::String (page))
         .withParameter ("output", "json");

    for (auto* f : fields)
        u = u.withParameter ("fl[]", f);

    return u;
}

juce::URL metadataUrl (const juce::String& identifier)
{
    return juce::URL ("https://archive.org/metadata/"
                        + juce::URL::addEscapeChars (identifier, false, false));
}

/* Percent-encode each PATH SEGMENT, leave the slashes alone. Fact 4 in the
 * header: real names carry spaces, brackets, parentheses and embedded
 * subdirectories, and both "encode everything" and "encode nothing" produce a
 * 404 on a file that is perfectly present. `dir` comes from /metadata and is
 * archive-generated ("/7/items/<identifier>"), so it needs no escaping. */
juce::URL fileUrl (const juce::String& node, const juce::String& dir,
                   const juce::String& name)
{
    juce::StringArray segs;
    segs.addTokens (name, "/", "");

    juce::StringArray enc;
    for (const auto& s : segs)
        enc.add (juce::URL::addEscapeChars (s, false, false));

    /* createWithoutParsing: the string is already exactly right, and letting
     * juce::URL parse it would re-mangle the escapes we just placed. */
    return juce::URL::createWithoutParsing ("https://" + node + dir + "/"
                                                + enc.joinIntoString ("/"));
}

/* Storage nodes, best first. NEVER /download -- see fact 2. */
juce::StringArray resolveNodes (const juce::var& meta)
{
    juce::StringArray out;
    auto add = [&out] (const juce::String& s)
    {
        const juce::String t = s.trim();
        if (t.isNotEmpty() && ! out.contains (t)) out.add (t);
    };

    add (prop (meta, "server").toString());
    for (const auto& s : asList (prop (meta, "workable_servers")))
        add (s);
    add (prop (meta, "d1").toString());
    add (prop (meta, "d2").toString());

    return out;
}

/* ---- transfer ------------------------------------------------------------ */
constexpr int kChunk = 64 * 1024;

bool downloadTo (const juce::URL& u, const juce::File& dest,
                 std::atomic<bool>& cancelFlag,
                 std::atomic<juce::int64>& got,
                 std::atomic<juce::int64>& total,
                 juce::String& errOut)
{
    GateHold hold (&cancelFlag);
    if (! hold.held) { errOut = "CANCELLED"; return false; }

    int status = 0;
    std::unique_ptr<juce::InputStream> in (
        u.createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withExtraHeaders (requestHeaders())
                .withConnectionTimeoutMs (30000)
                .withNumRedirectsToFollow (0)     // fact 2: never onto a CDN node
                .withStatusCode (&status)));

    if (in == nullptr)
    {
        errOut = "NO ANSWER";
        return false;
    }
    if (status != 0 && (status < 200 || status >= 300))
    {
        /* A 3xx here is the redirect we refused to follow, which is exactly
         * the failure this design is built to notice. Try the next node. */
        errOut = "HTTP " + juce::String (status);
        return false;
    }

    dest.getParentDirectory().createDirectory();
    dest.deleteFile();

    total.store (in->getTotalLength());
    got.store (0);

    bool ok = true;
    {
        juce::FileOutputStream out (dest);
        if (! out.openedOk())
        {
            errOut = "CANNOT WRITE INTO " + dest.getParentDirectory().getFileName().toUpperCase();
            return false;
        }

        juce::HeapBlock<char> buf (kChunk);
        for (;;)
        {
            if (cancelFlag.load()) { errOut = "CANCELLED"; ok = false; break; }

            const int n = in->read (buf.getData(), kChunk);
            if (n <= 0) break;

            if (! out.write (buf.getData(), (size_t) n))
            {
                errOut = "DISK WRITE FAILED";
                ok = false;
                break;
            }
            got.fetch_add ((juce::int64) n);
        }
        out.flush();
    }

    if (! ok)
        dest.deleteFile();

    return ok;
}

/* ---- ffmpeg --------------------------------------------------------------
 * juce::ChildProcess CANNOT WRITE TO A CHILD'S STDIN -- it exposes start,
 * isRunning, readProcessOutput, getExitCode and kill, and nothing else. Every
 * pipeline here therefore reads a real file off disk, which is why the source
 * is downloaded in full before ffmpeg is invoked rather than streamed into it.
 * -nostdin is passed as well, so a build of ffmpeg that would otherwise try to
 * read the console cannot hang holding a handle nobody can write to. */
juce::String ffmpegExe()
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable ("MORGUE_FFMPEG", {});
    if (env.trim().isNotEmpty())
        return env.trim();

    return "ffmpeg";        // resolved through PATH by the OS on every platform
}

bool transcode (const juce::File& src, const juce::File& dst,
                std::atomic<bool>& cancelFlag, juce::String& errOut)
{
    dst.getParentDirectory().createDirectory();
    dst.deleteFile();

    /* 44.1k mono 16-bit: what the engine renders at and what GRAIN MASS
     * expects. Identical to tools/exhume.py's transcode(). */
    juce::StringArray cmd;
    cmd.add (ffmpegExe());
    cmd.add ("-nostdin");
    cmd.add ("-v");           cmd.add ("error");
    cmd.add ("-y");
    cmd.add ("-i");           cmd.add (src.getFullPathName());
    cmd.add ("-vn");
    cmd.add ("-ac");          cmd.add ("1");
    cmd.add ("-ar");          cmd.add ("44100");
    cmd.add ("-c:a");         cmd.add ("pcm_s16le");
    cmd.add (dst.getFullPathName());

    juce::ChildProcess p;
    if (! p.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        errOut = "FFMPEG NOT ON PATH (SET MORGUE_FFMPEG)";
        return false;
    }

    /* Drained in a loop rather than with readAllProcessOutput(), which blocks
     * until the child exits and would make CANCEL a lie for the duration of a
     * long transcode. */
    juce::String out;
    while (p.isRunning())
    {
        if (cancelFlag.load())
        {
            p.kill();
            dst.deleteFile();
            errOut = "CANCELLED";
            return false;
        }

        char buf[1024];
        const int n = p.readProcessOutput (buf, (int) sizeof buf);
        if (n > 0) out += juce::String::fromUTF8 (buf, n);
        else       juce::Thread::sleep (40);
    }

    {
        char buf[1024];
        for (int n = p.readProcessOutput (buf, (int) sizeof buf); n > 0;
                 n = p.readProcessOutput (buf, (int) sizeof buf))
            out += juce::String::fromUTF8 (buf, n);
    }

    if (p.getExitCode() != 0)
    {
        const juce::String first = out.trim().upToFirstOccurrenceOf ("\n", false, false);
        errOut = first.isNotEmpty() ? first.toUpperCase().substring (0, 90)
                                    : juce::String ("FFMPEG FAILED");
        dst.deleteFile();
        return false;
    }

    if (! dst.existsAsFile() || dst.getSize() <= 44)
    {
        errOut = "FFMPEG WROTE NOTHING";
        return false;
    }

    return true;
}

/* ---- session paths (always asked of Session.h, never rebuilt) ------------
 * The transcoded specimen is written into the SESSION ROOT itself, not into a
 * subdirectory. That is deliberate: the LOCKER scans the root and only the
 * root, GRAIN MASS's chooser opens on the root, and an acquisition that lands
 * where the player cannot see it is an acquisition that will not get used.
 * Ledger::adopt() names it after its serial, so the root does not accumulate
 * files called "78_the-old-rugged-cross_1927 (2).wav".
 *
 * The raw downloaded original and the audition mp3s are cached OUT of the way,
 * because they are neither playable material nor evidence the player browses. */
juce::File sourceCacheDir (const juce::String& identifier)
{
    return morgue::morgueDir().getChildFile ("EXHUME")
                              .getChildFile ("source")
                              .getChildFile (identifier);
}

juce::File auditionCacheDir()
{
    return morgue::morgueDir().getChildFile ("EXHUME").getChildFile ("audition");
}

juce::String sizeText (juce::int64 bytes)
{
    if (bytes <= 0) return U8 ("\xe2\x80\x94");
    if (bytes < 1024LL * 1024LL)
        return juce::String (bytes / 1024LL) + "K";
    if (bytes < 1024LL * 1024LL * 1024LL)
        return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + "M";
    return juce::String ((double) bytes / (1024.0 * 1024.0 * 1024.0), 2) + "G";
}

/* The browse-time clearance mark. Same alphabet as the script prints.
 * Returns a string literal, not a pointer into a temporary -- U8() is applied
 * at the call site, because U8("...").toRawUTF8() would dangle the moment the
 * expression ended. */
const char* clearanceMark (Clearance c)
{
    switch (c)
    {
        case Clearance::Cleared:      return "+";
        case Clearance::Review:       return "?";
        case Clearance::PersonalOnly: return "X";
        default:                      return "\xc2\xb7";      // U+00B7, via U8()
    }
}

juce::Colour clearanceColour (Clearance c)
{
    switch (c)
    {
        case Clearance::Cleared:      return C::INK;         // ash: clear
        case Clearance::Review:       return C::OXIDE;       // oxide: verify per item
        case Clearance::PersonalOnly: return C::BLOOD_HOT;   // blood: never ships
        default:                      return C::INK_FAINT;
    }
}

} // anonymous namespace

/* ==========================================================================
 *  ArchiveFileEntry
 * ========================================================================== */
bool ArchiveFileEntry::isAudio() const
{
    return name.endsWithIgnoreCase (".mp3")  || name.endsWithIgnoreCase (".flac")
        || name.endsWithIgnoreCase (".wav")  || name.endsWithIgnoreCase (".ogg")
        || name.endsWithIgnoreCase (".m4a")  || name.endsWithIgnoreCase (".aiff")
        || name.endsWithIgnoreCase (".aif")  || name.endsWithIgnoreCase (".opus")
        || name.endsWithIgnoreCase (".wma");
}

bool ArchiveFileEntry::isVideo() const
{
    return name.endsWithIgnoreCase (".mp4")  || name.endsWithIgnoreCase (".mpeg")
        || name.endsWithIgnoreCase (".mpg")  || name.endsWithIgnoreCase (".avi")
        || name.endsWithIgnoreCase (".mkv")  || name.endsWithIgnoreCase (".mov")
        || name.endsWithIgnoreCase (".ogv")  || name.endsWithIgnoreCase (".webm");
}

/* ==========================================================================
 *  ResultTable -- the search results
 * ========================================================================== */
class ExhumePanel::ResultTable : public juce::Component,
                                 private juce::ListBoxModel
{
public:
    explicit ResultTable (ExhumePanel& p) : owner (p)
    {
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);
    }

    void refresh()
    {
        list.deselectAllRows();
        list.updateContent();
        if (getNumRows() > 0)
            list.scrollToEnsureRowIsOnscreen (0);
        repaint();
    }

    void resized() override { list.setBounds (getLocalBounds()); }

    int getNumRows() override { return (int) owner.docs.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) owner.docs.size()) return;
        const ArchiveDoc& d = owner.docs[(size_t) row];

        if (selected)
        {
            g.setColour (C::TAB_ACTIVE_BG);
            g.fillRect (0, 0, w, h);
        }

        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (0, h - 1, w, 1);

        Rectangle<int> r (0, 0, w, h);
        r.removeFromLeft (8);
        r.removeFromRight (8);

        // clearance mark -- the whole reason this column exists
        g.setColour (clearanceColour (d.clearance));
        g.setFont (Type::monoMedium (10.0f, 0.06f));
        g.drawText (U8 (clearanceMark (d.clearance)), r.removeFromLeft (12),
                    Justification::centredLeft);
        r.removeFromLeft (6);

        // date, right
        {
            const juce::Font f = Type::mono (8.0f);
            g.setFont (f);
            g.setColour (C::INK_FAINT);
            g.drawText (d.date.substring (0, 10), r.removeFromRight (66),
                        Justification::centredRight);
            r.removeFromRight (8);
        }

        // creator, right of the title when there is room for it
        if (r.getWidth() > 420)
        {
            g.setFont (Type::mono (8.0f));
            g.setColour (C::INK_FAINT);
            g.drawText (d.creator, r.removeFromRight (170), Justification::centredRight, true);
            r.removeFromRight (8);
        }

        // identifier -- the string you would type into the CLI
        const int idW = juce::jmin (230, juce::jmax (110, r.getWidth() / 3));
        g.setColour (selected ? C::BLOOD_HOT : C::INK_DIM);
        g.setFont (Type::mono (9.0f, 0.04f));
        g.drawText (d.identifier, r.removeFromLeft (idW), Justification::centredLeft, true);
        r.removeFromLeft (8);

        g.setColour (selected ? C::INK_BRIGHT : C::INK);
        g.setFont (Type::mono (10.0f));
        g.drawText (d.title.isNotEmpty() ? d.title : d.identifier, r,
                    Justification::centredLeft, true);
    }

    void selectedRowsChanged (int lastRow) override
    {
        /* One click opens the item, which costs one /metadata request. The
         * request is generation-guarded and passes through the same gate as
         * everything else, so arrowing down a list is throttled rather than
         * being a small denial-of-service against a charity. */
        owner.selectDoc (lastRow);
    }

    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) owner.docs.size()) return {};
        const ArchiveDoc& d = owner.docs[(size_t) row];
        return d.identifier + U8 (" \xe2\x80\x94 ") + d.clearanceNote
             + U8 (". Click to read the item record.");
    }

    juce::ListBox list { "exhume-results", this };
    ExhumePanel& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResultTable)
};

/* ==========================================================================
 *  FileTable -- one item's files
 * ========================================================================== */
class ExhumePanel::FileTable : public juce::Component,
                               private juce::ListBoxModel
{
public:
    explicit FileTable (ExhumePanel& p) : owner (p)
    {
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);
    }

    void refresh() { list.deselectAllRows(); list.updateContent(); repaint(); }
    int  selectedRow() const { return list.getSelectedRow(); }

    void resized() override { list.setBounds (getLocalBounds()); }

    int getNumRows() override { return (int) owner.item.files.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) owner.item.files.size()) return;
        const ArchiveFileEntry& f = owner.item.files[(size_t) row];

        if (selected)
        {
            g.setColour (C::TAB_ACTIVE_BG);
            g.fillRect (0, 0, w, h);
        }

        Rectangle<int> r (0, 0, w, h);
        r.removeFromLeft (6);
        r.removeFromRight (6);

        /* ORIGINAL vs DERIVATIVE, stated rather than implied. A derivative is
         * a lossy re-encode of the original; acquiring one when the original
         * is present is a choice, and the player should have to make it
         * knowingly. */
        g.setColour (f.isOriginal() ? C::OXIDE : C::INK_GHOST);
        g.setFont (Type::mono (7.0f, 0.12f));
        g.drawText (f.isOriginal() ? "ORIGINAL" : "DERIV", r.removeFromLeft (52),
                    Justification::centredLeft);

        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText (sizeText (f.size), r.removeFromRight (54), Justification::centredRight);
        r.removeFromRight (6);

        g.setColour (selected ? C::INK_BRIGHT : C::INK_DIM);
        g.setFont (Type::mono (9.0f));
        g.drawText (f.name, r, Justification::centredLeft, true);
    }

    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) owner.item.files.size()) return {};
        const ArchiveFileEntry& f = owner.item.files[(size_t) row];
        return f.name + U8 (" \xe2\x80\x94 ") + f.format
             + (f.md5.isNotEmpty() ? (" md5 " + f.md5) : juce::String())
             + U8 (". Select it to fetch this file instead of the best one.");
    }

    juce::ListBox list { "exhume-files", this };
    ExhumePanel& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileTable)
};

/* ==========================================================================
 *  ExhumePanel
 * ========================================================================== */

ExhumePanel::ExhumePanel (AudioEngine& e) : audio (e)
{
    setWantsKeyboardFocus (true);
    setTooltip (U8 ("EXHUME \xe2\x80\x94 acquisition from archive.org. Search, read the "
                    "item record, then FETCH: the file is downloaded, verified against "
                    "the archive's own md5, transcoded to 44.1k mono and accessioned "
                    "into the register with its licence and its clearance."));

    queryBox.setMultiLine (false);
    queryBox.setReturnKeyStartsNewLine (false);
    queryBox.setFont (Type::mono (11.0f));
    queryBox.setTextToShowWhenEmpty ("SEARCH THE ARCHIVE", C::INK_GHOST);
    queryBox.setTooltip (U8 ("QUERY \xe2\x80\x94 Lucene syntax, exactly as archive.org's "
                             "advanced search takes it. RETURN searches."));
    queryBox.onReturnKey = [this] { page = 1; runSearch (1); };
    addAndMakeVisible (queryBox);

    auto plate = [this] (PlateButton& b, const juce::String& tip, std::function<void()> fn)
    {
        b.setTooltip (tip);
        b.setMouseClickGrabsKeyboardFocus (false);
        b.onToggle = [fn] (bool) { if (fn) fn(); };
        addAndMakeVisible (b);
    };

    plate (searchBtn, U8 ("SEARCH \xe2\x80\x94 query advancedsearch.php. Never the scrape "
                          "API: unauthenticated, scrape reports 13 items for a collection "
                          "that has 10374, with no warning."),
           [this] { page = 1; runSearch (1); });

    plate (auditionBtn, U8 ("AUDITION \xe2\x80\x94 download this item's derivative mp3 into "
                            "the cache and play it. Nothing is accessioned."),
           [this] { startAudition(); });

    plate (fetchBtn, U8 ("FETCH \xe2\x80\x94 download, verify the md5, transcode to 44.1k "
                         "mono 16-bit, and accession into the register with full "
                         "provenance."),
           [this] { startFetch(); });

    plate (cancelBtn, U8 ("CANCEL \xe2\x80\x94 stop the transfer. A partial file is deleted; "
                          "nothing is accessioned."),
           [this] { cancelJob(); });

    plate (prevBtn, U8 ("PREVIOUS PAGE of results."),
           [this] { if (page > 1) { --page; runSearch (page); } });

    plate (nextBtn, U8 ("NEXT PAGE of results."),
           [this] { if (page * kRows < numFound) { ++page; runSearch (page); } });

    results   = std::make_unique<ResultTable> (*this);
    filesView = std::make_unique<FileTable> (*this);
    addAndMakeVisible (*results);
    addAndMakeVisible (*filesView);

    readAhead.startThread();

    searchLine = "NO QUERY. THE ARCHIVE DOES NOT VOLUNTEER.";
    detailLine = "SELECT A RESULT.";

    startTimerHz (30);
}

ExhumePanel::~ExhumePanel()
{
    stopTimer();

    /* Tell every worker to stop before anything else. They hold their own
     * shared_ptr to the state, so this is safe whether or not they are still
     * running, and their UI hop is SafePointer-guarded regardless. */
    if (jobState != nullptr)
        jobState->cancel.store (true);

    stopAudition();

    if (playerAttached)
    {
        audio.getManager().removeAudioCallback (&player);
        player.setSource (nullptr);
        playerAttached = false;
    }

    readAhead.stopThread (2000);
}

/* ---- geometry ----------------------------------------------------------- */

int ExhumePanel::chipsRowCount (int width) const
{
    /* Before the first resized() the panel has no width, and an unguarded wrap
     * would report fifteen rows and give the chip band the whole window. */
    if (width < 240)
        return 2;

    const juce::Font f = Type::mono (8.0f, 0.12f);
    int rows = 1, x = 8;
    const int right = juce::jmax (60, width - 8);

    for (int i = 0; i < kNumCollections; ++i)
    {
        const int w = textW (f, kCollections[i].label) + 16;
        if (x + w > right && x > 8) { ++rows; x = 8; }
        x += w + 4;
    }
    return rows;
}

void ExhumePanel::layoutChips (Rectangle<int> band,
                               std::vector<Rectangle<int>>& out) const
{
    out.clear();
    const juce::Font f = Type::mono (8.0f, 0.12f);
    int x = band.getX() + 8, y = band.getY() + 2;
    const int right = juce::jmax (band.getX() + 60, band.getRight() - 8);

    for (int i = 0; i < kNumCollections; ++i)
    {
        const int w = textW (f, kCollections[i].label) + 16;
        if (x + w > right && x > band.getX() + 8)
        {
            x = band.getX() + 8;
            y += kChipRowH;
        }
        out.push_back ({ x, y, w, 18 });
        x += w + 4;
    }
}

Rectangle<int> ExhumePanel::queryBand() const
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    return b.removeFromTop (kQueryH);
}

Rectangle<int> ExhumePanel::chipBand() const
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH + kQueryH);
    return b.removeFromTop (kChipRowH * chipsRowCount (getWidth()) + 4);
}

Rectangle<int> ExhumePanel::footBand() const
{
    return getLocalBounds().removeFromBottom (kFootH);
}

Rectangle<int> ExhumePanel::bodyArea() const
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH + kQueryH + chipBand().getHeight());
    b.removeFromBottom (kFootH);
    return b;
}

Rectangle<int> ExhumePanel::detailArea() const
{
    Rectangle<int> b = bodyArea();
    const int w = juce::jmin (400, juce::jmax (240, b.getWidth() * 45 / 100));
    return b.removeFromRight (w);
}

Rectangle<int> ExhumePanel::resultArea() const
{
    Rectangle<int> b = bodyArea();
    b.removeFromRight (detailArea().getWidth() + 1);       // + the divider rule
    return b;
}

Rectangle<int> ExhumePanel::mediaSegBounds (int i) const
{
    const Rectangle<int> q = queryBand();
    const int blockW = 3 * kSegW + 2 * kSegGap;
    const int x0 = q.getRight() - 8 - kBtnW - 10 - blockW;
    return { x0 + i * (kSegW + kSegGap), q.getY() + 6, kSegW, 20 };
}

/* ---- layout ------------------------------------------------------------- */

void ExhumePanel::resized()
{
    const Rectangle<int> q = queryBand();
    const int editorRight = mediaSegBounds (0).getX() - 10;
    queryBox.setBounds (q.getX() + 8, q.getY() + 5,
                        juce::jmax (80, editorRight - q.getX() - 8), 22);
    searchBtn.setBounds (q.getRight() - 8 - kBtnW, q.getY() + 5, kBtnW, 22);

    if (results == nullptr || filesView == nullptr)
        return;                                   // constructed later in the ctor

    results->setBounds (resultArea());

    Rectangle<int> d = detailArea();
    d.removeFromTop (20 + 84);                    // label row + the fixed meta block
    Rectangle<int> actions = d.removeFromBottom (56);
    d.removeFromTop (18);                         // "FILES" label row
    filesView->setBounds (d.reduced (6, 0));

    const int bw = juce::jmin (kBtnW, (actions.getWidth() - 32) / 3);
    const int by = actions.getY() + 26;
    auditionBtn.setBounds (actions.getX() + 8, by, bw, 22);
    fetchBtn   .setBounds (actions.getX() + 8 + bw + 6, by, bw, 22);
    cancelBtn  .setBounds (actions.getX() + 8 + 2 * (bw + 6), by, bw, 22);

    const Rectangle<int> f = footBand();
    nextBtn.setBounds (f.getRight() - 8 - 54, f.getY() + 3, 54, 18);
    prevBtn.setBounds (f.getRight() - 8 - 54 - 4 - 54, f.getY() + 3, 54, 18);
}

/* ---- query -------------------------------------------------------------- */

juce::String ExhumePanel::buildQuery() const
{
    juce::StringArray clauses;

    const juce::String q = queryBox.getText().trim();
    if (q.isNotEmpty())
        clauses.add ("(" + q + ")");

    if (collection > 0)
        clauses.add ("collection:(" + juce::String (kCollections[collection].id) + ")");

    if (mediaSel == 0) clauses.add ("mediatype:(audio)");
    if (mediaSel == 1) clauses.add ("mediatype:(movies)");

    return clauses.isEmpty() ? juce::String ("*:*") : clauses.joinIntoString (" AND ");
}

void ExhumePanel::runSearch (int pageIndex)
{
    page = juce::jmax (1, pageIndex);
    searchState = Search::Running;
    searchLine  = "QUERYING THE ARCHIVE";
    docs.clear();
    numFound = 0;
    selectedDoc = -1;
    item = ArchiveItem();
    detailState = Detail::None;
    detailLine  = "SELECT A RESULT.";
    results->refresh();
    filesView->refresh();
    repaint();

    const juce::String query = buildQuery();
    const int gen = ++searchGen;
    const int pg  = page;

    juce::Component::SafePointer<ExhumePanel> safe (this);

    juce::Thread::launch ([safe, query, pg, gen]
    {
        const NetResult res = getText (searchUrl (query, kRows, pg), nullptr);

        std::vector<ArchiveDoc> parsed;
        int found = 0;
        juce::String error = res.error;

        if (res.ok)
        {
            const juce::var root = juce::JSON::parse (res.body);
            const juce::var response = prop (root, "response");
            found = (int) prop (response, "numFound");

            if (auto* arr = prop (response, "docs").getArray())
            {
                for (const auto& d : *arr)
                {
                    ArchiveDoc doc;
                    doc.identifier = str (d, "identifier");
                    doc.title      = str (d, "title");
                    doc.creator    = str (d, "creator");
                    doc.date       = str (d, "date");
                    doc.mediatype  = str (d, "mediatype");
                    doc.licenceUrl = str (d, "licenseurl");
                    doc.collections = asList (prop (d, "collection"));
                    doc.clearance  = clearanceForCollections (doc.collections,
                                                              &doc.clearanceNote);
                    if (doc.identifier.isNotEmpty())
                        parsed.push_back (doc);
                }
            }
            else if (found == 0 && response.isVoid())
            {
                error = "THE ARCHIVE ANSWERED IN A SHAPE THIS CLIENT DOES NOT KNOW";
            }
        }

        juce::MessageManager::callAsync ([safe, parsed, found, error, gen]
        {
            if (safe == nullptr) return;
            if (gen != safe->searchGen.load()) return;     // a newer query won

            safe->docs     = parsed;
            safe->numFound = found;

            if (error.isNotEmpty())
            {
                safe->searchState = error.startsWith ("NO ROUTE") ? Search::Offline
                                                                  : Search::Failed;
                safe->searchLine  = error.startsWith ("NO ROUTE")
                    ? juce::String ("NO ROUTE TO ARCHIVE.ORG. THE ARCHIVE IS INTACT; "
                                    "THE NETWORK IS NOT.")
                    : error + ". NOTHING WAS ACQUIRED.";
            }
            else if (parsed.empty())
            {
                safe->searchState = Search::NoResults;
                safe->searchLine  = "NOTHING MATCHES. THE ARCHIVE IS LARGE; THE QUERY WAS NOT.";
            }
            else
            {
                safe->searchState = Search::Ready;
                safe->searchLine.clear();
            }

            safe->results->refresh();
            safe->repaint();
        });
    });
}

/* ---- item detail -------------------------------------------------------- */

void ExhumePanel::selectDoc (int row)
{
    if (row < 0 || row >= (int) docs.size())
        return;

    selectedDoc = row;
    fetchArmed  = false;
    item        = ArchiveItem();
    detailState = Detail::Loading;
    detailLine  = "READING THE ITEM RECORD";

    /* A PERSONAL_ONLY warning, a failure line or a serial from the last fetch
     * belongs to the item it was about. Carrying it onto the next one is how a
     * player reads "GREAT 78 PROJECT: PRIVATE STUDY ONLY" under a LibriVox
     * recording. Anything still running keeps its line. */
    if (! jobIsBusy())
        setJob (Job::None, {});

    filesView->refresh();
    repaint();

    openItem (docs[(size_t) row].identifier);
}

void ExhumePanel::openItem (const juce::String& identifier)
{
    const int gen = ++detailGen;
    juce::Component::SafePointer<ExhumePanel> safe (this);

    juce::Thread::launch ([safe, identifier, gen]
    {
        const NetResult res = getText (metadataUrl (identifier), nullptr);

        ArchiveItem out;
        juce::String error = res.error;

        if (res.ok)
        {
            const juce::var root = juce::JSON::parse (res.body);
            const juce::var md   = prop (root, "metadata");

            /* /metadata answers "{}" for an identifier that does not exist. It
             * does NOT 404, which is why the emptiness has to be tested for
             * rather than inferred from a status code. */
            if (md.isVoid() || str (md, "identifier").isEmpty())
            {
                error = "NO SUCH ITEM ON ARCHIVE.ORG";
            }
            else
            {
                out.identifier  = str (md, "identifier");
                out.title       = str (md, "title");
                out.creator     = str (md, "creator");
                out.date        = str (md, "date");
                out.uploader    = str (md, "uploader");
                out.licenceUrl  = str (md, "licenseurl");
                out.collections = asList (prop (md, "collection"));
                out.dir         = prop (root, "dir").toString();
                out.nodes       = resolveNodes (root);
                out.clearance   = clearanceForCollections (out.collections,
                                                           &out.clearanceNote);

                if (auto* arr = prop (root, "files").getArray())
                {
                    for (const auto& fv : *arr)
                    {
                        ArchiveFileEntry f;
                        f.name   = str (fv, "name");
                        f.source = str (fv, "source");
                        f.format = str (fv, "format");
                        f.md5    = str (fv, "md5");
                        f.size   = (juce::int64) str (fv, "size").getLargeIntValue();

                        if (f.name.isNotEmpty() && (f.isAudio() || f.isVideo()))
                            out.files.push_back (f);
                    }
                }

                /* pick_files()'s order, verbatim: originals first, then
                 * largest. A derivative is a lossy re-encode; if the original
                 * is there, it is what should be acquired. */
                std::stable_sort (out.files.begin(), out.files.end(),
                                  [] (const ArchiveFileEntry& a, const ArchiveFileEntry& b)
                                  {
                                      if (a.isOriginal() != b.isOriginal())
                                          return a.isOriginal();
                                      return a.size > b.size;
                                  });

                out.valid = true;
            }
        }

        juce::MessageManager::callAsync ([safe, out, error, gen]
        {
            if (safe == nullptr) return;
            if (gen != safe->detailGen.load()) return;

            if (! out.valid)
            {
                safe->detailState = Detail::Failed;
                safe->detailLine  = error.isNotEmpty() ? error
                                                       : juce::String ("NO ITEM RECORD.");
            }
            else
            {
                safe->item = out;
                safe->detailState = Detail::Ready;
                safe->detailLine.clear();
                if (out.files.empty())
                    safe->detailLine = "NO AUDIO OR VIDEO FILES IN THIS ITEM.";
            }

            safe->filesView->refresh();
            safe->repaint();
        });
    });
}

/* ---- file choice -------------------------------------------------------- */

const ArchiveFileEntry* ExhumePanel::chosenFile() const
{
    if (item.files.empty()) return nullptr;

    const int sel = filesView != nullptr ? filesView->selectedRow() : -1;
    if (sel >= 0 && sel < (int) item.files.size())
        return &item.files[(size_t) sel];

    return &item.files.front();       // already sorted originals-first
}

const ArchiveFileEntry* ExhumePanel::bestDerivativeMp3() const
{
    const ArchiveFileEntry* best = nullptr;
    for (const auto& f : item.files)
        if (! f.isOriginal() && f.name.endsWithIgnoreCase (".mp3"))
            if (best == nullptr || (f.size > 0 && f.size < best->size))
                best = &f;

    // some items ship only an original mp3; that is a fine audition too
    if (best == nullptr)
        for (const auto& f : item.files)
            if (f.name.endsWithIgnoreCase (".mp3"))
                if (best == nullptr || (f.size > 0 && f.size < best->size))
                    best = &f;

    return best;
}

void ExhumePanel::setJob (Job j, const juce::String& line)
{
    job = j;
    jobLine = line;
    fetchBtn.setLamp (jobIsBusy());
    repaint (detailArea());
}

bool ExhumePanel::jobIsBusy() const
{
    return job == Job::Downloading || job == Job::Verifying
        || job == Job::Transcoding || job == Job::Recording;
}

/* ---- FETCH --------------------------------------------------------------
 * Worker thread from here down. Nothing in this chain touches a component
 * except through MessageManager::callAsync with a SafePointer.
 * ------------------------------------------------------------------------- */

void ExhumePanel::startFetch()
{
    if (jobIsBusy())
        return;

    if (detailState != Detail::Ready || ! item.valid)
    {
        setJob (Job::Failed, "NO ITEM SELECTED.");
        return;
    }

    const ArchiveFileEntry* entry = chosenFile();
    if (entry == nullptr)
    {
        setJob (Job::Failed, "THIS ITEM HAS NO AUDIO OR VIDEO FILE.");
        return;
    }

    /* PERSONAL_ONLY items warn BEFORE a byte moves. The Great 78 Project's own
     * description says research, teaching and private study only, and a
     * warning that arrives after the download is a warning that arrives after
     * the player has already started building on it. Two clicks, no modal --
     * a modal for this would be the console interrupting itself, and the rule
     * is that nothing about a fetch opens a window. */
    if (item.clearance == Clearance::PersonalOnly && ! fetchArmed)
    {
        fetchArmed = true;
        setJob (Job::Warning, item.clearanceNote.toUpperCase()
                                + ". CLICK FETCH AGAIN TO ACCEPT. IT MUST NOT SHIP.");
        return;
    }
    fetchArmed = false;

    jobState = std::make_shared<JobState>();
    jobState->total.store (entry->size);
    lastPaintedGot = -1;
    setJob (Job::Downloading, "FETCHING");

    const ArchiveItem      it = item;      // copies: the worker owns its data
    const ArchiveFileEntry fe = *entry;
    auto st = jobState;

    juce::Component::SafePointer<ExhumePanel> safe (this);

    juce::Thread::launch ([safe, it, fe, st]
    {
        auto fail = [safe] (const juce::String& why)
        {
            juce::MessageManager::callAsync ([safe, why]
            {
                if (safe == nullptr) return;
                safe->setJob (why == "CANCELLED" ? Job::Cancelled : Job::Failed,
                              why == "CANCELLED"
                                  ? juce::String ("CANCELLED. NOTHING WAS WRITTEN.")
                                  : why);
            });
        };

        /* ---- 1. the original bytes, from a node we resolved ourselves ---- */
        const juce::File raw = sourceCacheDir (it.identifier)
                                   .getChildFile (juce::File::createLegalFileName (
                                       fe.name.fromLastOccurrenceOf ("/", false, false)));

        bool have = false;
        juce::String servedBy;

        if (raw.existsAsFile() && fe.md5.isNotEmpty()
            && juce::MD5 (raw).toHexString().equalsIgnoreCase (fe.md5))
        {
            have = true;                       // already here and provably intact
            servedBy = "CACHE";
        }

        juce::String lastError = "NO STORAGE NODES IN THE ITEM RECORD";

        for (int i = 0; ! have && i < it.nodes.size(); ++i)
        {
            if (st->cancel.load()) { fail ("CANCELLED"); return; }

            const juce::String node = it.nodes[i];
            juce::String err;

            if (! downloadTo (fileUrl (node, it.dir, fe.name), raw, st->cancel,
                              st->got, st->total, err))
            {
                if (err == "CANCELLED") { fail ("CANCELLED"); return; }
                lastError = node + ": " + err;
                continue;                       // fact 3: only a real transfer tells
            }

            /* ---- 2. verify against the archive's own md5 ---- */
            if (fe.md5.isNotEmpty())
            {
                juce::MessageManager::callAsync ([safe]
                {
                    if (safe != nullptr) safe->setJob (Job::Verifying, "VERIFYING MD5");
                });

                if (! juce::MD5 (raw).toHexString().equalsIgnoreCase (fe.md5))
                {
                    raw.deleteFile();
                    lastError = node + ": MD5 MISMATCH";
                    continue;                   // a bad node, not a bad file
                }
            }

            have = true;
            servedBy = node;
        }

        if (! have)
        {
            fail (juce::String ("ALL NODES FAILED ") + U8 ("\xe2\x80\x94 ") + lastError);
            return;
        }
        if (st->cancel.load()) { fail ("CANCELLED"); return; }

        /* ---- 3. mint the serial FIRST, then transcode straight to it -----
         * Ledger::adopt() is called with renameToSerial = FALSE, and that is
         * not the usual choice for an acquisition. The reason: adopt()'s
         * rename refuses to clobber, so handing it a file that is ALREADY
         * named <serial>.wav would make it "keep both" and produce
         * <serial>-2.wav. Minting here and writing ffmpeg's output directly to
         * the final name gets the same result -- a self-identifying file whose
         * name, ledger entry and LOCKER row are one string -- with no rename
         * step, and with no window in which a half-written file called
         * "78_the_old_rugged_cross_1927.wav" appears in the LOCKER. */
        auto& ledger = morgue::Ledger::shared();
        const juce::String serial = ledger.mint (morgue::Kind::Acq);
        const juce::File   wav    = morgue::morgueDir().getChildFile (serial + ".wav");

        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->setJob (Job::Transcoding, "TRANSCODING 44.1K MONO 16-BIT");
        });

        juce::String terr;
        if (! transcode (raw, wav, st->cancel, terr)) { fail (terr); return; }

        /* ---- 4. accession ------------------------------------------------
         * Three separate licence fields, because archive.org's licence
         * metadata is UPLOADER-SUPPLIED and the Archive does not vouch for it.
         * A record that says "CC0" without saying who said so is a rumour with
         * a URL attached. When nothing was declared, nothing is claimed --
         * `licence` and `declaredBy` both stay empty and the clearance state
         * carries the whole truth. */
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr) safe->setJob (Job::Recording, "ACCESSIONING");
        });

        morgue::Record proto;
        proto.serial   = serial;
        proto.creator  = it.creator;
        proto.date     = it.date;
        proto.source   = "https://archive.org/details/" + it.identifier;
        proto.sourceId = it.identifier;
        proto.tool     = "EXHUME/0.1 (MORGUE panel)";

        if (it.licenceUrl.isNotEmpty())
        {
            proto.licence    = it.licenceUrl;
            proto.declaredBy = it.uploader.isNotEmpty() ? ("uploader:" + it.uploader)
                                                        : juce::String ("uploader");
        }

        proto.clearance = clearanceForCollections (it.collections, &proto.note);

        proto.extra.set ("x-archive-file",        fe.name);
        proto.extra.set ("x-archive-md5",         fe.md5);
        proto.extra.set ("x-archive-format",      fe.format);
        proto.extra.set ("x-archive-source",      fe.source);
        proto.extra.set ("x-archive-node",        servedBy);
        proto.extra.set ("x-archive-collections", it.collections.joinIntoString (" "));
        proto.extra.set ("x-transcode",
                         "ffmpeg -vn -ac 1 -ar 44100 -c:a pcm_s16le");

        const juce::String origin = it.title.isNotEmpty() ? it.title : it.identifier;

        // WORKER THREAD ONLY -- adopt() hashes the file and fsyncs the line.
        const morgue::Record rec = ledger.adopt (wav, morgue::Kind::Acq, origin,
                                                 /* renameToSerial */ false, proto);

        const int stillPending = ledger.pendingCount();

        juce::MessageManager::callAsync ([safe, rec, stillPending]
        {
            if (safe == nullptr) return;

            safe->setJob (Job::Done,
                          "ACCESSIONED " + rec.serial
                            + (stillPending > 0
                                   ? juce::String (" (LEDGER WRITE PENDING)")
                                   : juce::String()));

            if (safe->onAcquired) safe->onAcquired();
        });
    });
}

/* ---- AUDITION -----------------------------------------------------------
 * A deliberate simplification, stated out loud rather than hidden: instead of
 * a Range-streaming AudioFormatReader over HTTP -- which fact 2 in the header
 * makes a minefield, since the nodes that break are exactly the ones that
 * break on Range -- AUDITION downloads the item's derivative mp3 to a cache
 * file and plays it with the AudioFormatReader the app already owns. The
 * derivative is single-digit to low-tens of megabytes. The cost is a wait with
 * a progress bar; the saving is an entire streaming reader that would have to
 * be correct about partial content, seek, and failover mid-file.
 * ------------------------------------------------------------------------- */

void ExhumePanel::startAudition()
{
    if (jobIsBusy())
        return;

    /* AUDITION is a toggle: a second press stops. There is no separate stop
     * plate because there is nothing else it could mean. */
    if (transport.isPlaying())
    {
        stopAudition();
        setJob (Job::None, {});
        return;
    }

    if (detailState != Detail::Ready || ! item.valid)
    {
        setJob (Job::Failed, "NO ITEM SELECTED.");
        return;
    }

    const ArchiveFileEntry* mp3 = bestDerivativeMp3();
    if (mp3 == nullptr)
    {
        setJob (Job::Failed, "NO MP3 DERIVATIVE TO AUDITION. FETCH IT INSTEAD.");
        return;
    }

    jobState = std::make_shared<JobState>();
    jobState->total.store (mp3->size);
    lastPaintedGot = -1;
    setJob (Job::Downloading, "CACHING DERIVATIVE");

    const ArchiveItem      it = item;
    const ArchiveFileEntry fe = *mp3;
    auto st = jobState;

    juce::Component::SafePointer<ExhumePanel> safe (this);

    juce::Thread::launch ([safe, it, fe, st]
    {
        const juce::File cached = auditionCacheDir()
            .getChildFile (juce::File::createLegalFileName (
                it.identifier + "-" + fe.name.fromLastOccurrenceOf ("/", false, false)));

        bool have = cached.existsAsFile() && fe.md5.isNotEmpty()
                 && juce::MD5 (cached).toHexString().equalsIgnoreCase (fe.md5);

        juce::String lastError = "NO STORAGE NODES IN THE ITEM RECORD";

        for (int i = 0; ! have && i < it.nodes.size(); ++i)
        {
            if (st->cancel.load()) break;

            juce::String err;
            if (downloadTo (fileUrl (it.nodes[i], it.dir, fe.name), cached,
                            st->cancel, st->got, st->total, err))
            {
                have = true;
                break;
            }
            if (err == "CANCELLED") break;
            lastError = it.nodes[i] + ": " + err;
        }

        const bool cancelled = st->cancel.load();

        juce::MessageManager::callAsync ([safe, cached, have, cancelled, lastError]
        {
            if (safe == nullptr) return;

            if (cancelled)
            {
                safe->setJob (Job::Cancelled, "CANCELLED. NOTHING WAS WRITTEN.");
                return;
            }
            if (! have)
            {
                safe->setJob (Job::Failed,
                              juce::String ("ALL NODES FAILED ")
                                  + U8 ("\xe2\x80\x94 ") + lastError);
                return;
            }

            safe->beginPlayback (cached);
        });
    });
}

void ExhumePanel::beginPlayback (const juce::File& f)
{
    stopAudition();

    std::unique_ptr<juce::AudioFormatReader> reader (audio.getFormats().createReaderFor (f));
    if (reader == nullptr)
    {
        setJob (Job::Failed, "THIS FILE WILL NOT DECODE.");
        return;
    }

    const double rate = reader->sampleRate;
    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    transport.setSource (readerSource.get(), 32768, &readAhead, rate);

    /* The player is attached lazily, on the first audition, and never before
     * AudioEngine::start() has run. JUCE's AudioDeviceManager gives the FIRST
     * registered callback the real output buffer and mixes every later one in
     * on top; attaching from this panel's constructor would have made the
     * audition the primary callback and the instrument the addition. */
    if (! playerAttached)
    {
        player.setSource (&transport);
        audio.getManager().addAudioCallback (&player);
        playerAttached = true;
    }

    auditionName = f.getFileName();
    transport.setPosition (0.0);
    transport.start();
    setJob (Job::None, {});
    repaint (detailArea());
}

void ExhumePanel::stopAudition()
{
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
    auditionName.clear();
}

void ExhumePanel::cancelJob()
{
    if (jobState != nullptr)
        jobState->cancel.store (true);

    if (transport.isPlaying())
        stopAudition();

    setJob (Job::Cancelled, "CANCELLED. NOTHING WAS WRITTEN.");
}

/* ---- the 30 Hz pull ------------------------------------------------------
 * Reads two atomics the workers write and repaints the detail pane only when
 * the byte count actually moved. No I/O, no allocation of consequence, and
 * nothing that can block the message thread. */
void ExhumePanel::sync()
{
    if (! jobIsBusy() || jobState == nullptr)
        return;

    const juce::int64 got = jobState->got.load();
    if (got != lastPaintedGot)
    {
        lastPaintedGot = got;
        repaint (detailArea());
    }
}

void ExhumePanel::timerCallback()
{
    /* The panel keeps its own timer so the progress readout and the audition
     * state stay live whether or not Main.cpp's sync() is wired. */
    sync();

    /* Repaint on the EDGE of the play state, not every frame while playing:
     * the audition line is a static string, and a 30 Hz repaint of a 400px
     * pane to redraw the same text is exactly the kind of free CPU cost that
     * shows up as a dropout under load. */
    const bool playing = transport.isPlaying();
    if (playing != lastPlaying)
    {
        lastPlaying = playing;
        repaint (detailArea());
    }
}

/* ---- input -------------------------------------------------------------- */

void ExhumePanel::selectCollection (int chipIndex)
{
    if (chipIndex < 0 || chipIndex >= kNumCollections) return;
    if (collection == chipIndex) return;

    collection = chipIndex;
    page = 1;
    repaint (chipBand());
    runSearch (1);
}

void ExhumePanel::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    for (int i = 0; i < 3; ++i)
        if (mediaSegBounds (i).contains (p))
        {
            if (mediaSel != i)
            {
                mediaSel = i;
                page = 1;
                repaint (queryBand());
                runSearch (1);
            }
            return;
        }

    std::vector<Rectangle<int>> chips;
    layoutChips (chipBand(), chips);
    for (int i = 0; i < (int) chips.size(); ++i)
        if (chips[(size_t) i].contains (p))
        {
            selectCollection (i);
            return;
        }
}

void ExhumePanel::mouseMove (const juce::MouseEvent& e)
{
    std::vector<Rectangle<int>> chips;
    layoutChips (chipBand(), chips);

    int h = -1;
    for (int i = 0; i < (int) chips.size(); ++i)
        if (chips[(size_t) i].contains (e.getPosition()))
            h = i;

    if (h != hoverChip) { hoverChip = h; repaint (chipBand()); }
}

void ExhumePanel::mouseExit (const juce::MouseEvent&)
{
    if (hoverChip != -1) { hoverChip = -1; repaint (chipBand()); }
}

bool ExhumePanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress (juce::KeyPress::returnKey))
    {
        page = 1;
        runSearch (1);
        return true;
    }
    return false;
}

/* ---- painting ----------------------------------------------------------- */

void ExhumePanel::paintChips (juce::Graphics& g)
{
    const Rectangle<int> band = chipBand();
    g.setColour (C::PANEL_ALT);
    g.fillRect (band);
    g.setColour (C::HAIRLINE);
    g.fillRect (band.getX(), band.getBottom() - 1, band.getWidth(), 1);

    std::vector<Rectangle<int>> chips;
    layoutChips (band, chips);

    g.setFont (Type::mono (8.0f, 0.12f));
    for (int i = 0; i < (int) chips.size() && i < kNumCollections; ++i)
    {
        const Rectangle<int> r = chips[(size_t) i];
        const bool on    = (i == collection);
        const bool hover = (i == hoverChip);

        g.setColour (on ? C::BLOOD_DEEP : (hover ? C::PLATE_HOVER : C::PLATE_LOW));
        g.fillRect (r);
        g.setColour (on ? C::BLOOD : C::HAIRLINE);
        g.drawRect (r, 1);
        g.setColour (on ? C::ARMED_TEXT : (hover ? C::INK : C::INK_DIM));
        g.drawText (kCollections[i].label, r, Justification::centred);
    }
}

void ExhumePanel::paintSegs (juce::Graphics& g)
{
    for (int i = 0; i < 3; ++i)
    {
        const Rectangle<int> seg = mediaSegBounds (i);
        const bool active = (i == mediaSel);

        g.setColour (active ? C::PLATE_HOVER : C::PLATE_LOW);
        g.fillRect (seg);
        g.setColour (active ? C::EDGE : C::HAIRLINE);
        g.drawRect (seg, 1);
        g.setColour (active ? C::INK : C::TAB_INACTIVE_FG);
        g.setFont (Type::mono (8.0f, 0.12f));
        g.drawText (kMediaNames[i], seg, Justification::centred);
    }
}

void ExhumePanel::paintDetail (juce::Graphics& g, Rectangle<int> d)
{
    const auto dot = U8 (" \xc2\xb7 ");

    g.setColour (C::PANEL);
    g.fillRect (d);
    g.setColour (C::HAIRLINE);
    g.fillRect (d.getX() - 1, d.getY(), 1, d.getHeight());

    // label row
    Rectangle<int> label = d.removeFromTop (20);
    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (8.0f, 0.16f));
    g.drawText ("ITEM RECORD", label.reduced (8, 0), Justification::centredLeft);
    g.setColour (C::HAIRLINE);
    g.fillRect (label.getX(), label.getBottom() - 1, label.getWidth(), 1);

    Rectangle<int> meta = d.removeFromTop (84);

    /* The action block is laid out and painted whatever the item state is --
     * the plates are child components and are always on screen, so drawing
     * their chrome only in the happy path would leave three buttons floating
     * on a bare panel with no rule above them. */
    const bool ready = (detailState == Detail::Ready);

    if (! ready)
    {
        g.setColour (detailState == Detail::Failed ? C::OXIDE : C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText (detailLine, meta.reduced (10, 0), Justification::centredLeft, true);
    }

    if (ready)
    {
        Rectangle<int> m = meta.reduced (10, 6);

        g.setColour (C::INK);
        g.setFont (Type::mono (10.0f));
        g.drawText (item.title.isNotEmpty() ? item.title : item.identifier,
                    m.removeFromTop (14), Justification::centredLeft, true);

        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText (item.creator + (item.date.isNotEmpty()
                                        ? (dot + item.date.substring (0, 10))
                                        : juce::String()),
                    m.removeFromTop (12), Justification::centredLeft, true);
        g.drawText (item.collections.joinIntoString (dot),
                    m.removeFromTop (12), Justification::centredLeft, true);

        /* Two lines about licence, not one, and never a bare licence string.
         * What was declared and WHO declared it are separate facts, because
         * archive.org licence metadata is uploader-supplied and the Archive
         * does not vouch for it. Then the clearance this project actually
         * holds, in its own colour. */
        g.setColour (item.licenceUrl.isNotEmpty() ? C::INK_DIM : C::INK_GHOST);
        g.drawText (item.licenceUrl.isNotEmpty()
                        ? ("DECLARED " + item.licenceUrl
                            + (item.uploader.isNotEmpty()
                                   ? (" BY UPLOADER " + item.uploader)
                                   : juce::String()))
                        : juce::String ("NO LICENCE DECLARED BY THE UPLOADER"),
                    m.removeFromTop (12), Justification::centredLeft, true);

        g.setColour (clearanceColour (item.clearance));
        g.setFont (Type::monoMedium (8.0f, 0.12f));
        g.drawText (juce::String (clearanceTag (item.clearance)) + dot
                        + item.clearanceNote.toUpperCase(),
                    m.removeFromTop (12), Justification::centredLeft, true);

        g.setColour (C::INK_GHOST);
        g.setFont (Type::mono (7.0f, 0.08f));
        g.drawText ("NODES " + item.nodes.joinIntoString (dot),
                    m.removeFromTop (10), Justification::centredLeft, true);
    }

    // FILES label row
    Rectangle<int> flabel = d.removeFromTop (18);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (7.0f, 0.14f));
    g.drawText (ready ? (juce::String ((int) item.files.size()) + " FILES"
                            + dot + "ORIGINALS FIRST")
                      : juce::String ("FILES"),
                flabel.reduced (8, 0), Justification::centredLeft);

    // the action block at the bottom (buttons are children; chrome is here)
    Rectangle<int> actions = d.removeFromBottom (56);
    g.setColour (C::HAIRLINE);
    g.fillRect (actions.getX(), actions.getY(), actions.getWidth(), 1);

    // progress trough + status line
    Rectangle<int> bar (actions.getX() + 8, actions.getY() + 8,
                        actions.getWidth() - 16, 6);

    const bool busy = jobIsBusy() && jobState != nullptr;

    if (busy)
    {
        const juce::int64 got = jobState->got.load(), total = jobState->total.load();
        g.setColour (C::TROUGH);
        g.fillRect (bar);
        g.setColour (C::HAIRLINE);
        g.drawRect (bar, 1);

        if (total > 0)
        {
            const int w = (int) ((double) (bar.getWidth() - 2)
                                    * juce::jlimit (0.0, 1.0, (double) got / (double) total));
            g.setColour (C::BLOOD);
            g.fillRect (bar.getX() + 1, bar.getY() + 1, w, bar.getHeight() - 2);
        }
    }

    juce::String line = jobLine;
    if (busy && jobState->total.load() > 0)
        line += " " + sizeText (jobState->got.load())
              + " / " + sizeText (jobState->total.load());
    else if (transport.isPlaying())
        line = "AUDITION " + auditionName;

    g.setColour (job == Job::Failed ? C::OXIDE
               : job == Job::Warning ? C::BLOOD_HOT
               : job == Job::Done ? C::INK
                                  : C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.08f));
    g.drawText (line, Rectangle<int> (actions.getX() + 8, actions.getY() + 16,
                                      actions.getWidth() - 16, 10),
                Justification::centredLeft, true);
}

void ExhumePanel::paint (juce::Graphics& g)
{
    const auto dot = U8 (" \xc2\xb7 ");

    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());

    paintHeaderBand (g, getLocalBounds().removeFromTop (headerBandH),
                     "EXHUME",
                     U8 ("ARCHIVE.ORG \xc2\xb7 ACQUISITION"),
                     juce::String (kSerial) + dot
                        + (numFound > 0 ? (juce::String (numFound) + " ITEMS FOUND")
                                        : juce::String ("ADVANCEDSEARCH.PHP")),
                     Badge::LIVE, "LIVE");

    // ---- query band -------------------------------------------------------
    const Rectangle<int> q = queryBand();
    g.setColour (C::PANEL_ALT);
    g.fillRect (q);
    g.setColour (C::HAIRLINE);
    g.fillRect (q.getX(), q.getBottom() - 1, q.getWidth(), 1);
    paintSegs (g);

    // ---- collection chips -------------------------------------------------
    paintChips (g);

    // ---- results ----------------------------------------------------------
    const Rectangle<int> res = resultArea();
    g.setColour (C::PANEL);
    g.fillRect (res);

    if (searchState != Search::Ready || docs.empty())
    {
        g.setColour (searchState == Search::Failed || searchState == Search::Offline
                         ? C::OXIDE : C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText (searchLine, res.reduced (24, 0), Justification::centred, true);
    }

    // ---- detail -----------------------------------------------------------
    paintDetail (g, detailArea());

    // ---- footer -----------------------------------------------------------
    Rectangle<int> f = footBand();
    g.setColour (C::PANEL_ALT);
    g.fillRect (f);
    g.setColour (C::HAIRLINE);
    g.fillRect (f.getX(), f.getY(), f.getWidth(), 1);

    Rectangle<int> fr = f.reduced (8, 0);

    // paging readout, left of the PREV/NEXT plates
    {
        const int first = docs.empty() ? 0 : (page - 1) * kRows + 1;
        const int last  = docs.empty() ? 0 : first + (int) docs.size() - 1;
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (docs.empty() ? juce::String ("PAGE " + juce::String (page))
                                 : (juce::String (first) + U8 ("\xe2\x80\x93")
                                        + juce::String (last) + " OF " + juce::String (numFound)),
                    Rectangle<int> (fr.getRight() - 240, fr.getY(), 120, fr.getHeight()),
                    Justification::centredRight);
    }

    // legend + contact note
    g.setFont (Type::mono (8.0f, 0.10f));
    int x = fr.getX();
    auto legend = [&] (Clearance c, const char* text)
    {
        g.setColour (clearanceColour (c));
        const juce::String s = juce::String (U8 (clearanceMark (c))) + " " + text;
        const int w = textW (Type::mono (8.0f, 0.10f), s) + 14;
        g.drawText (s, Rectangle<int> (x, fr.getY(), w, fr.getHeight()),
                    Justification::centredLeft);
        x += w;
    };
    legend (Clearance::Cleared,      "CLEAR");
    legend (Clearance::Review,       "VERIFY PER ITEM");
    legend (Clearance::PersonalOnly, "PERSONAL USE ONLY");
    legend (Clearance::Unreviewed,   "UNREVIEWED");

    if (contactAddress() == "unset-contact")
    {
        g.setColour (C::OXIDE);
        g.setFont (Type::mono (7.0f, 0.10f));
        g.drawText ("SET MORGUE_CONTACT SO THE ARCHIVE CAN REACH YOU",
                    Rectangle<int> (x + 10, fr.getY(), juce::jmax (0, fr.getRight() - 250 - x - 10),
                                    fr.getHeight()),
                    Justification::centredLeft, true);
    }
}

/* ---- tooltips ------------------------------------------------------------ */

juce::String ExhumePanel::getTooltip()
{
    const auto p = getMouseXYRelative();
    const auto dash = U8 (" \xe2\x80\x94 ");

    for (int i = 0; i < 3; ++i)
        if (mediaSegBounds (i).contains (p))
            return juce::String (kMediaNames[i]) + dash
                 + "restricts the search to this mediatype.";

    std::vector<Rectangle<int>> chips;
    layoutChips (chipBand(), chips);
    for (int i = 0; i < (int) chips.size(); ++i)
        if (chips[(size_t) i].contains (p))
            return (i == 0 ? juce::String ("ALL COLLECTIONS")
                           : juce::String (kCollections[i].id))
                 + dash
                 + (i == 0 ? juce::String ("no collection clause in the query.")
                           : juce::String ("restricts the search to this collection. "
                                           "Every identifier on this row has been "
                                           "verified to exist."));

    if (detailArea().contains (p))
        return "ITEM RECORD" + dash
             + "from https://archive.org/metadata/<identifier>. The storage nodes "
               "are resolved here; /download redirects are never followed.";

    return juce::SettableTooltipClient::getTooltip();
}

} // namespace morgue
