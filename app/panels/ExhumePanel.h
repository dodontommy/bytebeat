/* ExhumePanel.h -- EXHUME: the archive.org acquisition workspace.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS
 *
 * tools/exhume.py already does this job from a terminal, and it does it
 * correctly -- it was written against the live API and every awkward thing it
 * knows, it learned the hard way. This panel is that script with a face on it,
 * and the port is deliberately faithful: same endpoint, same field list, same
 * clearance rules, same node-resolution strategy, same politeness. Where this
 * file diverges from the script the divergence is commented and justified. If
 * you are about to "simplify" something here, read the script first.
 *
 * ---------------------------------------------------------------------------
 * THE FOUR ARCHIVE.ORG FACTS THIS PANEL IS BUILT AROUND
 *
 * 1. THE SCRAPE API LIES TO UNAUTHENTICATED CLIENTS. For collection:prelinger
 *    it reports total=13. advancedsearch.php reports numFound=10374 for the
 *    same query. Guests cannot reach scope=all, and the failure mode is an
 *    HTTP 200 with no warning of any kind -- you get a short list and no
 *    reason to doubt it. So: advancedsearch.php, always, everywhere.
 *
 * 2. NEVER FOLLOW /download REDIRECTS. https://archive.org/download/<id>/<file>
 *    302s onto a round-robin CDN node, and some of those nodes answer 500 to
 *    exactly the Range requests real playback depends on. A dn*.ca node
 *    returned 500 five times out of five on a file the item's primary server
 *    served as a clean 206. We resolve the storage node ourselves out of
 *    /metadata (server, workable_servers, d1, d2) and fail over across the
 *    candidates. Every request this panel makes sets numRedirectsToFollow(0)
 *    so the rule is enforced by the transport and not merely by convention.
 *
 * 3. HEAD IS NOT A HEALTH PROBE. The same bad node answers HEAD with 200 and a
 *    correct Content-Length while failing every subsequent GET. There is
 *    therefore no cheap pre-flight: the only way to know a node works is to
 *    read bytes off it, so failover happens on the real transfer.
 *
 * 4. FILENAMES ARE NOT TIDY. Real archive.org file names contain spaces,
 *    brackets, parentheses, ampersands and embedded subdirectories. Each PATH
 *    SEGMENT is percent-encoded and the slashes are left alone; encoding the
 *    whole string turns "78_foo/bar.flac" into a 404 and encoding nothing
 *    turns "What To Do [1951].mp3" into one.
 *
 * ---------------------------------------------------------------------------
 * POLITENESS IS A FEATURE, NOT A COURTESY
 *
 * archive.org is a donation-funded nonprofit, and a panel with a search box is
 * capable of generating far more traffic than a person typing commands. So,
 * exactly as the script does: a User-Agent that names MORGUE and carries a
 * contact address ($MORGUE_CONTACT), a hard cap on requests in flight, a
 * minimum interval between request starts, and Retry-After honoured on 429 and
 * 503. Getting throttled is a fine outcome. Getting the string
 * "MORGUE-EXHUME" blocked for every future user of this instrument is not.
 *
 * ---------------------------------------------------------------------------
 * THREADING
 *
 * Nothing here goes near the audio thread; bb_engine_render() does not know
 * this panel exists. Every network read, every hash, every ffmpeg invocation
 * and every ledger write happens on a juce::Thread::launch worker in the shape
 * Chrome.cpp::growSpecimen() established, and comes back to the UI through
 * MessageManager::callAsync guarded by a juce::Component::SafePointer. The
 * message thread runs the 30 Hz engine sync and must never wait on a socket.
 *
 * sync() is the 30 Hz pull. It reads worker-written std::atomics (progress,
 * byte counts) and repaints only when one of them changed. It performs no I/O.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Theme.h"
#include "Primitives.h"
#include "Ledger.h"

class AudioEngine;

namespace morgue
{

/* ==========================================================================
 *  ArchiveDoc -- one row of advancedsearch.php's response.docs
 *
 *  Note that `creator` and `collection` come back as EITHER a string OR an
 *  array of strings depending on the item, which is why the parser normalises
 *  both through the same as-list helper the script uses.
 * ========================================================================== */
struct ArchiveDoc
{
    juce::String identifier, title, creator, date, mediatype, licenceUrl;
    juce::StringArray collections;

    /* Resolved at parse time, not at fetch time. The whole point of showing a
     * clearance mark in the result list is that the question is answered while
     * the player is still browsing -- after a track has been built on a
     * specimen, "personal use only" is no longer information, it is a
     * demolition order. */
    Clearance    clearance = Clearance::Unreviewed;
    juce::String clearanceNote;
};

/* ==========================================================================
 *  ArchiveFileEntry -- one entry of /metadata's files[]
 * ========================================================================== */
struct ArchiveFileEntry
{
    juce::String name;      // may contain '/', spaces, brackets
    juce::String source;    // "original" | "derivative" | "metadata"
    juce::String format;    // "VBR MP3", "Flac", "MPEG4", ...
    juce::String md5;       // the archive's own checksum; this is the verifier
    juce::int64  size = 0;

    bool isOriginal() const noexcept { return source.equalsIgnoreCase ("original"); }
    bool isAudio() const;
    bool isVideo() const;
};

/* ==========================================================================
 *  ArchiveItem -- /metadata/<identifier>
 * ========================================================================== */
struct ArchiveItem
{
    juce::String identifier, title, creator, date, uploader, licenceUrl;
    juce::String dir;                       // "/7/items/<id>" -- from /metadata
    juce::StringArray collections;
    juce::StringArray nodes;                // storage nodes, best first
    std::vector<ArchiveFileEntry> files;    // originals first, then largest

    Clearance    clearance = Clearance::Unreviewed;
    juce::String clearanceNote;
    bool valid = false;
};

/* ==========================================================================
 *  ExhumePanel
 * ========================================================================== */
class ExhumePanel : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    /* AudioEngine is here for AUDITION only: the panel adds its own
     * AudioSourcePlayer to the device manager, which JUCE mixes on top of the
     * engine callback. It never touches the engine's own state. */
    explicit ExhumePanel (AudioEngine&);
    ~ExhumePanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::String getTooltip() override;

    /* 30 Hz engine-sync slot (Main.cpp). Reads worker atomics; no I/O. */
    void sync();

    /* Fired on the message thread once an ACQ record has landed and the
     * transcoded WAV is in the session root, so the console can re-scan the
     * LOCKER. The specimen is written into the session root deliberately --
     * see the note above acquisitionDir() in the .cpp. */
    std::function<void()> onAcquired;

private:
    class ResultTable;                  // ListBox + model over `docs`
    class FileTable;                    // ListBox + model over `item.files`
    friend class ResultTable;
    friend class FileTable;

    /* ---- panel-level state machines ------------------------------------
     * Every one of these has a deadpan line attached to it. A network
     * condition NEVER opens a modal -- the console does not stop working
     * because a nonprofit's load balancer is having an afternoon. */
    enum class Search  { Idle, Running, Ready, NoResults, Offline, Failed };
    enum class Detail  { None, Loading, Ready, Failed };
    enum class Job     { None, Warning, Downloading, Verifying, Transcoding,
                         Recording, Done, Failed, Cancelled };

    /* ---- one running transfer, owned by shared_ptr ----------------------
     * The workers write `got` and read `cancel`, and a worker can outlive this
     * component: a 40 MB flac download does not stop because the player
     * switched tabs and the console shut down. So the progress counters do NOT
     * live in the panel. They live here, behind a shared_ptr the worker holds
     * a copy of, and the panel reads whatever is current. Putting these three
     * atomics in the component and passing references to a thread would be a
     * use-after-free with a stopwatch on it. */
    struct JobState
    {
        std::atomic<bool>        cancel { false };
        std::atomic<juce::int64> got    { 0 };
        std::atomic<juce::int64> total  { 0 };
    };

    /* ---- actions (all launch workers) ---- */
    void runSearch (int page);
    void openItem (const juce::String& identifier);
    void startFetch();
    void startAudition();
    void beginPlayback (const juce::File&);          // message thread
    void cancelJob();
    void stopAudition();

    /* ---- helpers ---- */
    void selectDoc (int row);
    void selectCollection (int chipIndex);
    juce::String buildQuery() const;
    const ArchiveFileEntry* chosenFile() const;      // selection, else best
    const ArchiveFileEntry* bestDerivativeMp3() const;
    void setJob (Job, const juce::String& line);
    bool jobIsBusy() const;
    void layoutChips (juce::Rectangle<int> band,
                      std::vector<juce::Rectangle<int>>& out) const;
    int  chipsRowCount (int width) const;

    /* ---- geometry ---- */
    juce::Rectangle<int> queryBand() const;
    juce::Rectangle<int> chipBand() const;
    juce::Rectangle<int> bodyArea() const;
    juce::Rectangle<int> detailArea() const;
    juce::Rectangle<int> resultArea() const;
    juce::Rectangle<int> footBand() const;
    juce::Rectangle<int> mediaSegBounds (int i) const;

    void timerCallback() override;
    void paintDetail (juce::Graphics&, juce::Rectangle<int>);
    void paintChips (juce::Graphics&);
    void paintSegs (juce::Graphics&);

    AudioEngine& audio;

    /* ---- query controls ---- */
    juce::TextEditor queryBox;
    PlateButton searchBtn   { "SEARCH",   false, false };
    PlateButton auditionBtn { "AUDITION", false, false };
    PlateButton fetchBtn    { "FETCH",    true,  false };   // lamp = transfer live
    PlateButton cancelBtn   { "CANCEL",   false, false };
    PlateButton prevBtn     { "PREV",     false, false };
    PlateButton nextBtn     { "NEXT",     false, false };

    int  mediaSel   = 0;                // 0 AUDIO / 1 MOVIES / 2 ANY
    int  collection = 0;                // index into kCollections; 0 = ALL
    int  page       = 1;
    int  hoverChip  = -1;

    /* ---- results ---- */
    std::unique_ptr<ResultTable> results;
    std::vector<ArchiveDoc> docs;
    int   numFound   = 0;
    int   selectedDoc = -1;
    Search searchState = Search::Idle;
    juce::String searchLine;            // the deadpan line for the current state

    /* Every search and every item open carries a generation. A slow reply for
     * a query the player has already replaced must not overwrite the list --
     * without this, typing three queries quickly leaves whichever server was
     * slowest in charge of what is on screen. */
    std::atomic<int> searchGen { 0 };
    std::atomic<int> detailGen { 0 };

    /* ---- item detail ---- */
    std::unique_ptr<FileTable> filesView;
    ArchiveItem item;
    Detail detailState = Detail::None;
    juce::String detailLine;

    /* ---- the job (fetch / audition) -------------------------------------
     * The destructor sets jobState->cancel and walks away: the worker's UI hop
     * is already SafePointer-guarded, so the only thing left to do is stop it
     * wasting the archive's bandwidth on a window that has closed. */
    Job job = Job::None;
    juce::String jobLine;
    std::shared_ptr<JobState> jobState;
    juce::int64 lastPaintedGot = -1;

    /* PERSONAL_ONLY items arm before they fetch: the first FETCH click states
     * the restriction, the second accepts it. Two clicks, no modal. */
    bool fetchArmed = false;

    /* ---- audition ------------------------------------------------------
     * Deliberate simplification, stated out loud: rather than build a
     * Range-streaming AudioFormatReader over HTTP (which fact 2 above makes a
     * minefield), AUDITION downloads the item's DERIVATIVE mp3 into a cache
     * directory and plays the local file with the AudioFormatReader the app
     * already has. Derivatives are single-digit to low-tens of megabytes; the
     * wait is a progress bar, not an architecture.
     *
     * The decode goes through AudioEngine's OWN AudioFormatManager rather than
     * a second one of ours: it is already registered, and on Windows the mp3
     * path comes from WindowsMediaAudioFormat while on macOS it comes from
     * CoreAudioFormat -- two different answers that registerBasicFormats()
     * already got right once. */
    juce::TimeSliceThread readAhead { "EXHUME audition" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioSourcePlayer player;
    bool playerAttached = false;
    bool lastPlaying = false;
    juce::String auditionName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExhumePanel)
};

} // namespace morgue
