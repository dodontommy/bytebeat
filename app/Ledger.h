/* Ledger.h -- ACCESSION: real serials and an append-only provenance register.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * MORGUE makes files. It grows specimens, it captures clips, it records the
 * master bus, and -- once EXHUME and the visual wing land -- it will pull audio
 * off archive.org, photograph plates, and take material out through hardware on
 * another machine and bring it back in. Every one of those artefacts is
 * evidence, and evidence that cannot be traced back to what it came from is
 * worth nothing at release time. You cannot credit a sample you cannot name,
 * and you cannot clear a licence you never wrote down.
 *
 * Before this file, identity in MORGUE was carried by FILENAMES, and the
 * filenames were not up to the job:
 *
 *   engine.c:1889   snprintf(name, "SPC-%04X.wav", seed & 0xFFFFu)
 *   engine.c:1940   snprintf(name, "SPC-V%02d-%04X.wav", layer + 1, seed & 0xFFFFu)
 *
 * The specimen synthesiser takes a full 32-bit seed and prints SIXTEEN BITS of
 * it into the name -- and that name is the ONLY place the seed is recorded
 * anywhere in the program. Two problems fall out of that one line. First, the
 * seed is destroyed: nothing on disk or in memory can ever reproduce the
 * specimen, so the render is not reproducible even in principle. Second, the
 * name is a 16-bit identifier, and 16 bits collide. By the birthday bound the
 * probability that N distinct specimens are all uniquely named is
 * approximately exp(-N^2 / 2^17); that crosses 50% at N = 302. A player who
 * grows three hundred specimens -- an afternoon's work -- more likely than not
 * has two different sounds sharing an identity, and because the writer does not
 * check for existence, the second render silently overwrites the first. Any
 * provenance graph built on those names contains edges that point at the wrong
 * sound, and a credits list generated from it is a false statement about
 * someone else's work.
 *
 *   app/panels/Chrome.cpp:489   g.drawText (juce::String (row + 1)...)
 *
 * And in the LOCKER, where the UI presents a column that reads exactly like an
 * accession number, what is actually painted is the ListBox ROW INDEX. It is
 * not an identifier at all. It renumbers every time a file lands in the
 * directory, because the list is sorted newest-first, so the number beside a
 * specimen changes whenever the player grows another one. A serial that changes
 * is worse than no serial, because the player will write it down.
 *
 * This header replaces both with one thing: a register. Serials are minted
 * here, once, and recorded here, immediately, alongside everything that would
 * otherwise have to be reconstructed later from a filename and a hope.
 *
 * ---------------------------------------------------------------------------
 * THE ROUND TRIP THIS IS REALLY FOR
 *
 * The reason derived_from is the most important field in the record is the
 * analog round trip. A finished piece of MORGUE material looks like this:
 *
 *   ACQ-260805-K7J4QWMR   audio acquired from archive.org (an item, a file,
 *                         an uploader, a declared licence nobody vouches for)
 *      -> SPC-260805-4TB0ZC9H   cut and re-synthesised in the instrument
 *         -> SCN-260806-QQ7M2XKD   played out through hardware and recaptured
 *            -> PLT-260806-J3W1H5RN  rendered into the plate that ships
 *
 * Four records deep. The thing that ships is the plate; the thing that has to
 * be credited and cleared is the acquisition four steps back. If the chain is
 * not written down at the moment each step happens it will not be
 * reconstructable afterwards -- nobody remembers, in November, which of two
 * hundred specimens fed a particular pass through a particular pedal. So each
 * record names its immediate parents, ancestry() walks the chain, and credits()
 * turns a set of shipped serials into the list of people who are owed a line.
 *
 * ---------------------------------------------------------------------------
 * THE FILE FORMAT, AND WHY IT IS NOT JSON
 *
 * <session root>/ACCESSION.ledger, one record per line, append-only.
 *
 * This project already has a config dialect: session.conf, written by
 * bb_config_save() in engine.c, is line-oriented `key value` plain text with
 * `#` comments and a `version` line, and its header says "plain text, edit it
 * if you like". A second dialect would be a second parser, a second set of
 * escaping bugs, and a second thing to explain. So the ledger speaks the same
 * grammar, with one addition: because a record must be ONE line (so that the
 * file is append-only in the strong sense, and so that `grep` returns whole
 * records), the `key value` pairs of a record are packed onto that line
 * separated by TABs.
 *
 *   serial ACQ-260805-K7J4QWMR<TAB>utc 2026-08-05T14:22:09Z<TAB>kind ACQ<TAB>
 *   origin What To Do In A Gas Attack<TAB>derived_from -<TAB>sha256 3f2a...
 *
 * Every property that matters follows from that shape:
 *
 *   greppable      `grep ACQ-260805-K7J4QWMR ACCESSION.ledger` returns the
 *                  whole record, not a fragment of one. `grep 'clearance
 *                  PERSONAL_ONLY'` returns everything that must not ship.
 *   diffable       append-only + one line per record means every diff of this
 *                  file is a pure addition. There is never a modified line to
 *                  review, so a suspicious diff is self-evidently suspicious.
 *   mergeable      two machines that both accession material produce two files
 *                  whose union is the correct merged ledger. Sort and dedupe by
 *                  serial and you are done -- no conflict resolution, no
 *                  ordering semantics to preserve. This is the property the
 *                  coming cross-machine sync is built on, and it is exactly the
 *                  property JSON and SQLite do not have. A JSON array cannot be
 *                  appended to without rewriting its closing bracket, which
 *                  means every write risks the whole file; and an SQLite file
 *                  is a binary blob that git cannot merge, cannot diff, and
 *                  cannot be repaired with a text editor at 2am.
 *   hand-editable  a wrong `origin` or a clearance that has since been
 *                  researched can be fixed with any editor. That is a feature.
 *                  It also means the parser must assume the file has been
 *                  edited by a human and cope -- see the cycle guard in
 *                  ancestry() and the tolerant record parser.
 *
 * ---------------------------------------------------------------------------
 * THREADING
 *
 * Nothing here goes anywhere near the audio thread; bb_engine_render() does not
 * know this file exists. But the ledger does file I/O, fsync and SHA-256, and
 * none of those may happen on the message thread, which runs the 30 Hz engine
 * sync and every paint routine.
 *
 *   append(), adopt(), loadNow(), sha256OfFile()   WORKER THREADS ONLY.
 *   appendAsync(), loadAsync()                     call from anywhere; they do
 *                                                  the work on a worker and
 *                                                  call back on the message
 *                                                  thread, in the shape
 *                                                  Chrome.cpp::growSpecimen()
 *                                                  established.
 *   lookup(), findByFile(), ancestry(), listByKind(), credits(), size()
 *                                                  safe from the message
 *                                                  thread. They read an
 *                                                  in-memory snapshot under a
 *                                                  lock that is only ever held
 *                                                  for a vector push or an
 *                                                  index probe -- never across
 *                                                  file I/O. See Ledger.cpp.
 *
 * Callers of the *Async methods own the lifetime problem: the completion runs
 * later, on the message thread, and the panel that asked for it may be gone.
 * Capture a juce::Component::SafePointer, exactly as growSpecimen() does.
 */

#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace morgue
{

/* ==========================================================================
 *  KIND -- the serial vocabulary
 *
 *  Three-letter tags, because they are read off a printed evidence tag and
 *  because the LOCKER's serial column is narrow. The first two extend
 *  conventions already in the tree rather than replacing them (engine.c grows
 *  SPC-, ArrangePanel captures CLIP-), so the vocabulary stays one vocabulary.
 * ========================================================================== */
enum class Kind
{
    Spc,        // SPC  grown specimen -- bb_engine_render_specimen*()
    Clip,       // CLIP arrangement capture -- ArrangePanel
    Rec,        // REC  live master-bus recording -- AudioEngine::WavRecorder
    Acq,        // ACQ  acquired audio -- EXHUME, archive.org and friends
    Scn,        // SCN  scanned or physically captured image -- the visual wing
    Plt,        // PLT  rendered plate -- PlatePanel / tools/degrade.py
    Rcp,        // RCP  recipe: a reproducible set of parameters, not a file
    Exp,        // EXP  export / release master -- ExportSheet
    Unknown
};

const char* kindTag (Kind) noexcept;              // "ACQ"
Kind        kindFromTag (juce::StringRef) noexcept;
const char* kindDescription (Kind) noexcept;      // "ACQUIRED AUDIO" -- for UI

/* ==========================================================================
 *  CLEARANCE
 *
 *  These four states, their spellings and their precedence are ported verbatim
 *  from tools/exhume.py (CLEARANCE_RULES / clearance_for). That script's rules
 *  were written against the real collections and the real archive.org
 *  behaviour; divergence between the script and the app would mean a specimen
 *  fetched by the CLI and one fetched by the panel disagreeing about whether
 *  the same item can ship. See clearanceForCollections() in Ledger.cpp for the
 *  rule table itself, which is a line-for-line port.
 * ========================================================================== */
enum class Clearance
{
    Cleared,        // CLEARED        safe to release
    Review,         // REVIEW         plausibly fine; a human must check the item
    PersonalOnly,   // PERSONAL_ONLY  study only. Never in a release.
    Unreviewed      // UNREVIEWED     no rule applied. Absence of knowledge.
};

const char* clearanceTag (Clearance) noexcept;    // "PERSONAL_ONLY"
Clearance   clearanceFromTag (juce::StringRef) noexcept;

/* Apply exhume.py's CLEARANCE_RULES to an item's collection list. Returns the
 * winning state and, through `noteOut`, the human sentence that goes with it --
 * a clearance state with no explanation is not actionable, and the note is what
 * the credits sheet prints. */
Clearance clearanceForCollections (const juce::StringArray& collections,
                                   juce::String* noteOut = nullptr);

/* Combine two clearance states for PROVENANCE PROPAGATION -- the state a
 * derived work inherits from a parent.
 *
 * Note carefully that this is NOT the same ordering exhume.py uses to pick a
 * winner among collection rules, and the difference is deliberate. In
 * exhume.py, UNREVIEWED ranks LAST (rank 3) because there it is a sentinel
 * meaning "no rule matched yet" -- any actual rule, including CLEARED, should
 * replace it. Here UNREVIEWED means something else: a real ancestor whose
 * status nobody established. A plate derived from one cleared source and one
 * unknown source is NOT cleared, so for propagation the order is
 *
 *      PERSONAL_ONLY  >  REVIEW  >  UNREVIEWED  >  CLEARED
 *
 * i.e. ignorance is more restrictive than permission. Getting this backwards is
 * how a release ships an unlicensed sample while the tooling reports green. */
Clearance moreRestrictive (Clearance a, Clearance b) noexcept;

/* ==========================================================================
 *  RECORD -- one line of the ledger
 * ========================================================================== */
struct Record
{
    /* ---- identity ---- */
    juce::String serial;        // ACQ-260805-K7J4QWMR. Minted here, never reused.
    juce::String utc;           // ISO-8601 UTC, second granularity, always Z.
    Kind         kind = Kind::Unknown;

    /* ---- what a human calls it ----------------------------------------
     * REQUIRED, and append() rejects a record without it. The LOCKER must
     * never be in a position to show a player a bare serial or a hash and
     * nothing else; if we cannot say what a thing IS at the moment we
     * accession it, we will not be able to say later either. */
    juce::String origin;

    /* ---- the chain ------------------------------------------------------
     * Serials of the immediate parents. Usually one; a mix or a montage has
     * several; an acquisition has none. Stored space-separated (serials
     * contain no spaces, by construction). */
    juce::StringArray derivedFrom;

    /* ---- the artefact ---------------------------------------------------
     * `sha256` is the content identity and survives renaming, copying and the
     * cross-machine trip. `file` is a path RELATIVE TO THE SESSION ROOT when
     * the artefact lives under it, absolute otherwise -- so a ledger synced to
     * another machine still resolves, which an absolute C:\Users\... path would
     * not. Both may be empty: an RCP record is a recipe, not a file. */
    juce::String sha256;
    juce::String file;

    /* ---- credit ---------------------------------------------------------- */
    juce::String creator;       // "United States. Office of Civil Defense"
    juce::String date;          // the work's own date, not the accession date
    juce::String source;        // canonical URL a human can open
    juce::String sourceId;      // archive.org identifier, catalogue no., etc.

    /* ---- licence --------------------------------------------------------
     * `declaredBy` is not decoration. archive.org licence metadata is
     * UPLOADER-SUPPLIED and the Archive explicitly does not vouch for it, so a
     * record that says "CC0" without saying who said so is a rumour with a URL
     * attached. Store the claim and the claimant separately, always. */
    juce::String licence;       // licenceurl or an SPDX-ish string
    juce::String declaredBy;    // "uploader:pdxpat", "archive.org", "operator"
    Clearance    clearance = Clearance::Unreviewed;
    juce::String note;          // the sentence that explains the clearance

    /* ---- provenance of the record itself ---------------------------------
     * What made this. "EXHUME/0.1", "bb_engine_render_specimen_voice",
     * "degrade.py". When a chain turns out to be wrong, this is what tells you
     * which piece of code to go and read. */
    juce::String tool;

    /* ---- everything else -------------------------------------------------
     * Kind-specific fields. Name them with an "x-" prefix by convention, so
     * that they can never collide with a reserved key a later version of this
     * file adds; a key that IS reserved today is dropped on write rather than
     * silently overwriting the member it shares a name with.
     *
     * Unknown keys read out of the file land here too, and are written back
     * verbatim, which is what makes the format forward-compatible: an old
     * build round-trips a new build's records instead of destroying them.
     *
     * The convention that fixes the engine.c defect described at the top of
     * this file lives here:
     *
     *      x-seed  <the FULL 32-bit seed, hex>
     *
     * The seed is no longer only in the filename, and no longer only sixteen
     * bits of it, so a specimen is reproducible and its identity no longer
     * depends on a name that collides. */
    juce::StringPairArray extra;

    /* A record is well-formed if it can be found again and described to a
     * human: a syntactically valid serial and a non-empty origin. */
    bool isValid() const;

    /* The wire form, without its trailing newline. */
    juce::String toLine() const;

    /* Tolerant parser: unknown keys are preserved into `extra`, malformed
     * fields are skipped rather than aborting the line, and a line without a
     * `serial` key yields a record whose serial is empty (the caller drops it).
     * The file is hand-editable, so the parser must never throw a whole
     * ledger away because one line got mangled. */
    static Record fromLine (const juce::String&);
};

/* ==========================================================================
 *  CREDIT -- one entry in the artefact that makes all of this pay off
 * ========================================================================== */
struct Credit
{
    juce::String serial;
    juce::String origin;
    juce::String creator;
    juce::String date;
    juce::String source;
    juce::String licence;
    juce::String declaredBy;
    Clearance    clearance = Clearance::Unreviewed;
    juce::String note;
    juce::StringArray usedBy;   // which of the requested serials depend on this
};

/* ==========================================================================
 *  LEDGER
 * ========================================================================== */
class Ledger
{
public:
    /* One register per session root, so every panel writes to the same file
     * without Main.cpp having to thread a reference through eight
     * constructors. Constructed on first call, which is always after Main.cpp
     * has planted the engine's root -- the path is resolved lazily, at load
     * time, precisely so that ordering cannot bite. */
    static Ledger& shared();

    /* Kick the initial parse onto a worker. Call once from Main.cpp after the
     * root is planted; every query below answers from an empty register until
     * it completes, which is correct behaviour, not a race. */
    static void bootstrap();

    /* <session root>/ACCESSION.ledger, asked of Session.h. Never rebuilt. */
    juce::File file() const;

    /* ---- loading ---- */
    bool loadNow();                                 // WORKER THREAD ONLY
    void loadAsync (std::function<void (int numRecords)> done = {});
    bool isLoaded() const;
    int  size() const;

    /* ---- minting --------------------------------------------------------
     * See the note on entropy in Ledger.cpp. Format:
     *
     *      <KIND>-<YYMMDD>-<8 Crockford base32 chars>
     *      ACQ-260805-K7J4QWMR
     *
     * The date makes the serial sortable and legible on a printed tag; the
     * 40 bits of suffix make collision a non-issue. Freshly minted serials are
     * additionally checked against the loaded register and re-minted on the
     * (astronomically unlikely) local collision. */
    juce::String mint (Kind) const;

    /* ---- writing --------------------------------------------------------
     * WORKER THREAD ONLY -- this opens a file, writes and fsyncs.
     *
     * Fills in `serial` and `utc` if they are empty, so the common caller is
     * one line. Returns false and leaves the record QUEUED IN MEMORY if the
     * write fails; see pendingCount()/flushPending(). The record is always in
     * the in-memory register on return, success or failure, so a lookup
     * immediately afterwards resolves either way. */
    bool append (Record&);

    /* Same, off the message thread, with the completion delivered back on it.
     * The callback receives success and the completed record (serial filled
     * in). Capture a juce::Component::SafePointer in it -- see the threading
     * note at the top of this file. */
    void appendAsync (Record, std::function<void (bool, const Record&)> done = {});

    /* Hash a file, mint a serial, optionally rename the file to its serial,
     * and append. WORKER THREAD ONLY. This is the one call EXHUME and the
     * visual wing need.
     *
     * `renameToSerial` is not defaulted, on purpose, because it is a real
     * decision with two right answers. True is right for grown specimens and
     * acquisitions, where the existing name is either a colliding 16-bit hex
     * string (see the top of this file) or an archive.org filename full of
     * spaces and brackets -- renaming to the serial makes the file
     * self-identifying and ends the collision. False is right for arrangement
     * clips, whose CLIP-<LANE>-<NNNN> names are referenced by the arrangement
     * and would break if moved.
     *
     * `proto` pre-fills anything the caller already knows -- creator, source,
     * licence, derivedFrom, tool, extras -- and everything left empty is filled
     * in here. Returns the completed record, whose serial is always set even if
     * the WRITE failed; ask pendingCount() about durability. */
    Record adopt (const juce::File&, Kind, const juce::String& origin,
                  bool renameToSerial, Record proto = {});

    /* ---- reading (safe from the message thread) ---- */
    bool lookup (const juce::String& serial, Record& out) const;
    bool findByFile (const juce::File&, Record& out) const;
    bool findBySha256 (const juce::String&, Record& out) const;

    /* The chain, nearest parent first, self EXCLUDED. Breadth-first, so a
     * record with two parents lists both before either grandparent. Cycles and
     * runaway depth are guarded -- the file is hand-editable and someone will
     * eventually write a loop into it. */
    std::vector<Record> ancestry (const juce::String& serial) const;

    /* Records that name `serial` as a parent -- one generation down. */
    std::vector<Record> children (const juce::String& serial) const;

    std::vector<Record> listByKind (Kind) const;
    std::vector<Record> all() const;

    /* The most restrictive clearance anywhere in a record's ancestry,
     * including its own. This is the number that decides whether a plate can
     * ship, and it is nearly always worse than the record's own field. */
    Clearance effectiveClearance (const juce::String& serial) const;

    /* ---- the payoff -----------------------------------------------------
     * Given the serials that are actually shipping, walk every chain to its
     * roots and return the deduplicated list of things that are owed a credit,
     * each annotated with which shipped serials depend on it. Roots first --
     * the acquisition gets the line, not the eighth-generation render of it. */
    std::vector<Credit> credits (const juce::StringArray& shippedSerials) const;

    /* The same list as a plain-text block, in the register's own voice, with a
     * NOT CLEARED section at the end listing everything that must be resolved
     * or pulled before release. That trailing section is the point: a credits
     * sheet that only lists the easy cases is how the hard ones ship. */
    static juce::String creditsText (const std::vector<Credit>&);

    /* ---- durability ---- */
    int  pendingCount() const;      // records held in memory but not on disk
    bool flushPending();            // WORKER THREAD ONLY. Retry those writes.

    /* ---- helpers ---- */
    static juce::String sha256OfFile (const juce::File&);   // WORKER THREAD ONLY
    static juce::String isoUtcNow();
    /* Path as it is stored in `file`: relative to the session root when it
     * lives under it, absolute otherwise. */
    static juce::String relativeToRoot (const juce::File&);
    juce::File resolve (const Record&) const;               // the inverse

    Ledger() = default;

private:
    /* Deliberately no JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR: the shared
     * instance is a function-local static and is destroyed at exit in an order
     * unspecified relative to JUCE's own leak counters, which is exactly the
     * situation in which the detector fires a false positive. Copying is still
     * forbidden. */
    Ledger (const Ledger&) = delete;
    Ledger& operator= (const Ledger&) = delete;

    void indexLocked (int recordIndex);
    bool writeLine (const juce::String& line);      // one line, locked, fsynced

    /* Two locks, and the split is the whole reason the message thread stays
     * responsive. `writeLock` is held across an entire append -- serialising
     * file writes so that the order of lines on disk is the order of append()
     * calls -- and is taken ONLY by worker threads. `dataLock` guards the
     * in-memory register and is held only for a vector push, a hash-map insert
     * or a container swap, never across file I/O. A paint routine calling
     * lookup() can therefore wait on `dataLock` for the duration of a
     * push_back and nothing longer. */
    juce::CriticalSection writeLock;

    mutable juce::CriticalSection dataLock;         // guards everything below
    std::vector<Record> records;
    juce::HashMap<juce::String, int> bySerial;      // serial   -> index
    juce::HashMap<juce::String, int> byHash;        // sha256   -> index
    std::vector<juce::String> pending;              // lines not yet on disk
    bool loaded = false;
    mutable juce::String cachedPath;                // resolved on first file()
};

} // namespace morgue
