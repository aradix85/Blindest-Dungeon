
#pragma once
// ---- THE CLAUDE BRIDGE (fork-only; compiled in only with /DAX_BRIDGE, see build.ps1 -Bridge) ----
// A loopback HTTP server so an assistant can follow along with the player: every line handed to
// the screen reader, the debug-log stream, the active mod context, and keys pressed through the
// mod's own router. Without AX_BRIDGE every hook below compiles away and the DLL is unchanged.
#ifdef AX_BRIDGE
#include <cstdint>

void bridgeStart();                                   // speech thread, once the hooks are live
void bridgePump(uintptr_t base);                      // OurPoll (main thread), every frame
void bridgeNoteSpeech(const char* text, bool interrupt); // speakUtf8: a line handed to the reader
void bridgeNotePosted(const char* text, int kind);    // postSpeech: a line queued (with its kind)
void bridgeNoteLog(const char* line);                 // logLine / logDump, log file on or off
void bridgeNoteContext(const char* name);             // traceAxContext, every frame
bool bridgeMuted();                                   // speakUtf8 skips the reader while true
int  synthKeyQueued();                                // input/synth.cpp: keys still waiting
#endif
