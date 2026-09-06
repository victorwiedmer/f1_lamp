/*
 * F1Reaction.h  –  Pure, Arduino-free logic for deciding which F1 events
 * should change the LED state.  Extracted from F1NetWork.cpp so the exact
 * rules can be unit-tested on the host (no ESP32 SDK required).
 *
 * Data-model background (observed on the live feed in 2026):
 *   - TrackStatus codes CAN be stale (the feed stayed "2/Yellow" through
 *     safety-car and red-flag periods), so the LED also reacts to global
 *     RaceControl messages, which are what the TV mirrors.
 *   - RaceControlMessages entries carry Category ("Flag"/"SafetyCar"/…),
 *     Flag ("GREEN"/"YELLOW"/"DOUBLE YELLOW"/"RED"/"CLEAR"), Scope
 *     ("Track"/"Sector") and a human "Message" string.
 *   - Sector-scope entries are local marshalling notes ("YELLOW IN TRACK
 *     SECTOR 13") – only used when the user opts in via react_sector.
 */
#pragma once

#include "F1NetWork.h"   /* F1NetState enum (pure C, no Arduino) */
#include <cstring>
#include <cctype>

/* Case-insensitive substring test. */
inline bool f1_contains_ci(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == nl) return true;
    }
    return false;
}

/* Word-bounded, case-insensitive phrase test: the match must start and end
   on non-alphanumeric boundaries.  Prevents e.g. "CHEQUERED FLAG" from
   matching "RED FLAG" (CHEQUE<RED FLAG>). */
inline bool f1_contains_word(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return false;
    const size_t nl = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        if (p != hay && isalnum((unsigned char)p[-1])) continue;  /* start boundary */
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == nl && !(p[nl] && isalnum((unsigned char)p[nl]))) return true;
    }
    return false;
}

/* Map a TrackStatus code string to an F1NetState. */
inline F1NetState f1_trackCodeToState(const char* code)
{
    if (!code || !code[0]) return F1ST_UNKNOWN;
    switch (code[0]) {
        case '1': return F1ST_GREEN;
        case '2': return F1ST_YELLOW;
        case '3': return F1ST_YELLOW;      /* flag/yellow variant           */
        case '4': return F1ST_SAFETY_CAR;
        case '5': return F1ST_RED_FLAG;
        case '6': return F1ST_VIRTUAL_SC;
        case '7': return F1ST_VSC_ENDING;  /* VSC ending – brief green run   */
        default:  return F1ST_UNKNOWN;
    }
}

/*
 * Decide which LED state a RaceControl message drives.
 *
 *   reactGlobal – honour global announcements (RED FLAG / SAFETY CAR / VSC /
 *                 CHEQUERED) and track-scope flags
 *   reactSector – treat sector-local yellow notes as yellow
 *
 * Returns F1ST_UNKNOWN when the message must not change the LED.  Green is
 * deliberately left to the TrackStatus code 1: RC "TRACK CLEAR"-style notes
 * proved ambiguous in the live feed.
 */
inline F1NetState f1_rcEventToState(bool reactGlobal, bool reactSector,
                                    const char* cat, const char* flag,
                                    const char* scope, const char* text)
{
    if (!reactGlobal && !reactSector) return F1ST_UNKNOWN;

    /* Global announcements (these are what the TV mirrors) */
    if (reactGlobal && text && text[0]) {
        if (f1_contains_word(text, "RED FLAG"))        return F1ST_RED_FLAG;
        if (f1_contains_word(text, "VIRTUAL SAFETY CAR")
                || f1_contains_word(text, "VSC"))
            return F1ST_VIRTUAL_SC;
        if (f1_contains_word(text, "SAFETY CAR"))      return F1ST_SAFETY_CAR;
        if (f1_contains_word(text, "CHEQUERED"))       return F1ST_CHEQUERED;
    }

    bool isFlag = (cat && cat[0] && strcasecmp(cat, "Flag") == 0);
    if (!isFlag) return F1ST_UNKNOWN;

    bool isSector = (scope && strcasecmp(scope, "Sector") == 0);
    if (isSector) {
        /* Sector-local yellows only when the user opted in */
        if (reactSector && flag && f1_contains_ci(flag, "YELLOW"))
            return F1ST_YELLOW;
        return F1ST_UNKNOWN;
    }
    /* Track-scope flags */
    if (reactGlobal) {
        if (flag && f1_contains_ci(flag, "RED"))    return F1ST_RED_FLAG;
        if (flag && f1_contains_ci(flag, "YELLOW")) return F1ST_YELLOW;
    }
    return F1ST_UNKNOWN;
}
