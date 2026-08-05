/* Session.h -- where MORGUE keeps its things, asked once.
 *
 * The engine owns the answer. bb_config_set_root() plants the root and
 * bb_config_path() prints the session file underneath it; everything the
 * console shows or writes -- the LOCKER listing, the REC destination, the
 * ARRANGE captures, the path in the title bar -- has to agree with that one
 * answer or the player is told one thing and handed another.
 *
 * Before this header six call sites rebuilt "$HOME/MORGUE" by hand and a
 * seventh painted the literal string "~/MORGUE/session.conf" into the title
 * bar. On macOS and Linux the two happened to coincide. On Windows they do
 * not: the home directory is not spelled "~", the separator is not "/", and
 * a player reading the title bar would go looking for a directory that does
 * not exist. Ask here instead, and there is only ever one answer.
 *
 * Header-only on purpose: no new translation unit for the build to learn.
 */

#pragma once

#include <juce_core/juce_core.h>

#include "bytebeat.h"          /* bb_config_path() */

namespace morgue
{

/* The session file itself -- <root>/session.conf, exactly the file
 * bb_config_save() renames into place. The engine composes it with forward
 * slashes on every platform; juce::File normalises those on the way in, so
 * the mixed "C:\Users\x\MORGUE/session.conf" the engine hands back comes out
 * of getFullPathName() as a proper native path. */
inline juce::File sessionFile()
{
    const juce::String p = juce::String::fromUTF8 (bb_config_path());

    /* bb_config_path() invents an XDG-style relative fallback if it is asked
     * before a root has been planted, and juce::File asserts on a relative
     * path. The console always plants its root first (Main.cpp), so this is
     * belt and braces -- but a debug assert firing out of a paint routine is
     * a bad way to find that out. */
    if (! juce::File::isAbsolutePath (p))
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile ("MORGUE").getChildFile ("session.conf");

    return juce::File (p);
}

/* The directory the console lives out of: the session file, the REC
 * recordings, the grown specimens and the ARRANGE captures all sit here. */
inline juce::File morgueDir()
{
    return sessionFile().getParentDirectory();
}

/* The same directory as the player should read it. macOS and Linux keep the
 * familiar tilde form the design copy was written in; Windows has no such
 * convention, so the real path is printed and the player can paste it into
 * Explorer. */
inline juce::String morgueDirDisplay()
{
    const juce::String full = morgueDir().getFullPathName();

   #if defined (_WIN32)
    return full;
   #else
    const juce::String home = juce::File::getSpecialLocation (
        juce::File::userHomeDirectory).getFullPathName();
    if (home.isNotEmpty() && full.startsWith (home))
        return "~" + full.substring (home.length());
    return full;
   #endif
}

/* The platform's path separator as a String -- "/" or "\". juce::File hands
 * it back as a StringRef, which is one implicit conversion away from every
 * concatenation that wants it; naming it once keeps that conversion out of
 * expressions where it is easy to get wrong. */
inline juce::String pathSep()
{
    return juce::String (juce::File::getSeparatorString());
}

/* "~/MORGUE/session.conf" on a Mac, "C:\Users\...\MORGUE\session.conf" on
 * Windows. The title bar prints this. */
inline juce::String sessionFileDisplay()
{
    return morgueDirDisplay() + pathSep() + sessionFile().getFileName();
}

} // namespace morgue
