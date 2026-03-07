#pragma once
/*
 * F1Sessions.h  –  Fetch and cache the F1 session index from the
 *                  live-timing static API so the WebUI can show
 *                  past (and upcoming) sessions.
 *
 * Source : https://livetiming.formula1.com/static/{year}/Index.json
 */

#include <Arduino.h>

/* Fetch (or refresh) the session index.  Performs an HTTPS GET,
   parses the JSON, and caches a trimmed version in RAM.
   BLOCKING – call from a FreeRTOS task, never from the web-server
   callback (AsyncWebServer runs on the lwIP/WiFi task and TLS
   operations would deadlock).  Returns true on success. */
bool f1sessions_fetch(int year = 2026);

/* Request an async fetch.  Sets an internal flag that the f1net
   task picks up on its next loop iteration (avoids concurrent TLS
   connections which exhaust lwIP sockets on ESP32-C3).
   Safe to call from the web-server callback.  No-op if a fetch is
   already in progress or data is already cached. */
void f1sessions_requestFetch(int year = 2026);

/* true if a fetch has been requested but not yet started by f1net. */
bool f1sessions_fetchRequested();

/* true while a fetch is requested or in progress. */
bool f1sessions_isFetching();

/* true once at least one successful fetch has been done. */
bool f1sessions_hasData();

/* Return the cached JSON string (trimmed to meetings / sessions).
   Empty string if no data yet.  Caller must NOT free the result. */
const String& f1sessions_json();

/* Free the cached data (on OOM pressure or shutdown). */
void f1sessions_clear();

/* Last error message from a failed fetch (for diagnostics). */
const String& f1sessions_lastError();

/* Reset the retry counter (allows re-triggering after failures). */
void f1sessions_resetRetries();

/* ── Session replay: fetch .jsonStream files & drive the replay engine ── */

/* Request fetching a past session's streams for replay.
   Sets a flag picked up by the f1net task.  Safe to call from
   the web-server callback. */
void f1sessions_requestReplay(const char* sessionPath, float speed);

/* true if a replay fetch has been requested but not yet started. */
bool f1sessions_replayRequested();

/* true while the f1net task is downloading session streams for replay. */
bool f1sessions_isReplayFetching();

/* Fetch the 3 .jsonStream files, parse events, and start the replay
   engine.  BLOCKING – call from the f1net task only. */
bool f1sessions_fetchAndReplay();
