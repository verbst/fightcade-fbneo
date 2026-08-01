// Groovy MiSTer - the sender. See groovy_output.h.
//
// This file and groovy_input.cpp are the ONLY places that reach groovymister.h (and
// therefore <winsock2.h>), both via the module-internal groovy_internal.h. Keep it that way
// - see src/dep/groovymister/PROVENANCE.md.
//
// Threading: none. FBNeo renders into a CPU buffer, so there is no GPU readback to hide and
// nothing to gain from a sender thread. Everything here runs on the emulation thread, which
// is exactly the synchronous shape the Groovy client is designed around: one thread owns
// CmdInit / CmdSwitchres / CmdBlit / CmdAudio / WaitSync.

#include "burner.h"

#include "groovy_output.h"
#include "groovy_config.h"
#include "groovy_switchres.h"
#include "groovy_pixels.h"
#include "groovy_modeline.h"
#include "groovy_log.h"

#include "groovy_internal.h"	// declares the shared `gm` instance defined below

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For the raw CMD_CLOSE socket below. groovymister.h brings in <winsock2.h> on Windows and
// <sys/socket.h>/<netinet/in.h> on POSIX, but not these.
#ifndef _WIN32
#include <arpa/inet.h>	// inet_addr
#include <unistd.h>		// close
#include <errno.h>
#endif

using namespace GroovyMiSTer;

// Shared with groovy_input.cpp via groovy_internal.h. Not static for that reason.
GroovyMister gm;


static bool   bSessionOpen  = false;	// CmdInit succeeded
static bool   bStreaming    = false;	// a modeline is in effect and blits are going out

// Sticky for the life of the process: did we EVER get a session up? The close path gates on
// this rather than on bSessionOpen, because the whole point of that path is to be immune to
// confusion about the current state.
static bool   bEverConnected = false;

// Has this session's CMD_CLOSE already gone out? Exactly one is allowed - see the long comment
// above GroovySendRawCmd() for why a second one is pointless (the core now ignores it).
static bool   bRawCloseSent  = false;

// *** A deliberate close must STAY closed. ***
//
// Without this, GroovySessionClose() was immediately undone by the next rendered frame:
// it sets nLastConnectFailMs = 0, SessionRetryDue() reads 0 as "never failed, retry now", and
// GroovyFrameReady() re-opens on that basis. Nothing stops frames the instant we close -
// OnClose calls PostQuitMessage(0), which only lands when the loop next dequeues WM_QUIT, and
// QuarkEnd sets bMediaExit, which run.cpp:641 only acts on inside if (PeekMessage(...)). So the
// live sequence could be close -> re-open -> exit, leaving the core connected and displaying
// our last frame with no further close.
//
// Cleared only on an edge that genuinely means "a new session is wanted" - see the top of
// GroovyFrameReady(). NOT set by the internal SessionClose("params changed") reconnect, which
// must still be able to come straight back up.
static bool   bShutdownRequested = false;

// The endpoint we ACTUALLY opened with. The close goes here rather than to whatever the config
// currently says, so editing the host (or applying the settings dialog) mid-session cannot leave
// us closing a session on one address that we opened on another.
static char   szOpenHost[64] = "";
static INT32  nOpenPort      = 0;

// *** Keepalive against the core's idle timeout. ***
//
// The core closes a session that sends nothing for its configured interval (OSD Server -> Idle
// timeout: 5s default / 10 / 15 / Off) and frees the CRT. That is the safety net for a client that
// is killed or crashes - but it also fires when we are alive and merely quiet, which offline
// happens the moment a menu opens: OnEnterIdle (scrn.cpp:3154) only pumps RunIdle when kNetGame, so
// a Win32 menu or modal dialog stops everything.
//
// nLastWireMs is the timestamp of the last thing we actually put on the wire - updated by the blit
// AND the audio path. The keepalive fires only once that goes stale, so at 60fps it never fires at
// all: no extra packets while emulating, which is the whole design constraint.
static UINT32 nLastWireMs = 0;

// 2s against a 5s core default, well inside the handoff's "<= timeout/2". The TICK that drives it
// must be faster than this threshold or a tick landing just under it defers the send by a whole
// period - see GroovyKeepAlive(). At 2000 + 250 the worst-case silence is 2.25s normally and 4.5s
// if a single keepalive is lost, both under 5s. Surviving one lost datagram is exactly what the
// halving rule is for.
#define GROOVY_KEEPALIVE_IDLE_MS 2000

// *** Dead-session detection. ***
//
// The core no longer holds a session forever, so we can now be disconnected without being told. If
// we keep blitting into that, two bad things happen - one to the core, one to us:
//
//   The core's setClose() frees poc and deliberately does NOT null it (groovy.cpp:967-969, because
//   the logo path dereferences it), and CMD_BLIT is not gated on isConnected. So our blits write
//   through freed memory and sendACK() reads it back - the core hands us garbage frameEcho/vCount.
//
//   That garbage reaches DiffTimeRaster(), which turns it into an enormous diffRaster, and
//   WaitSync's loop does sleepTime += diffRaster on EVERY iteration. A large positive sleepTime
//   makes that busy-spin run for minutes: the reported "FBNeo freezes in the menu, needs a
//   force-kill".
//
// The vendored autoreconnect watchdog cannot catch this. Its test is "frameEcho > lastSeen", and
// garbage that happens to be larger reads as progress - so it resets its counter and never fires,
// in precisely the case it is most needed. Hence the second test below: not "did the echo change"
// but "does the echo make sense given what we sent". Garbage cannot satisfy that.
static UINT32 nLastEchoSeen   = 0;
static UINT32 nNoEchoBlits    = 0;
static UINT32 nBadEchos       = 0;
static UINT32 nLastReconEpoch = 0;
static bool   bSessionDead    = false;

// Well above the client's own 10-blit watchdog so it gets first refusal on a transient fault.
#define GROOVY_DEAD_BLITS 60

// How far the echo may lead what we sent before it stops being an acknowledgement. Generous: the
// core normally runs one frame BEHIND us (measured at 2041/2042 samples), so any lead at all is
// already odd and this only has to reject nonsense.
#define GROOVY_ECHO_SLACK 16

// Consecutive implausible echoes before we act, so one corrupt or reordered datagram cannot kill a
// healthy session.
#define GROOVY_BAD_ECHOS 5

// *** The reconnect freeze, and why we reset the client's own state. ***
//
// CmdInit re-zeroes NONE of the client's 29 session-scoped fields - they are initialised in the
// GroovyMister constructor only (groovymister.cpp:108-111, :155). Four of them drive the raster
// servo: fpga.frame, fpga.frameEcho, fpga.vCount, fpga.vCountEcho. They therefore survive every
// reconnect, ours and the client's internal one alike.
//
// That is what hard-locked FBNeo on a cable-pull. SessionOpen() correctly resets nBlitFrame to 0,
// but the resync in GroovyFrameReady() immediately adopted the DEAD session's fpga.frame (5472),
// so we blitted frame 5473 into a core whose counter had restarted at ~1. DiffTimeRaster() then
// computed vCount1 = 5472*262 against vCount2 = 262, dif = 716635, and
// diffTime = m_widthTime(640) * dif = 4.59e8 ticks - 46 SECONDS of busy-spin inside WaitSync, per
// echo change, accumulating. Hence the force-kill.
//
// Generally: m_frameTime = m_widthTime * vTotal (groovymister.cpp:845-846), so the implied spin is
// (m_frameTime * spread) / 2 - half a frame period for every frame of divergence between frameEcho
// and frame. Normal operation is a spread of 1.
//
// fpga is public, so we can do exactly what the constructor does. Call this on every session start.
// Defined below, once nBlitFrame exists.
static void ResetClientFrameState();

// Above this many frames of (frameEcho - frame), refuse to hand the state to WaitSync at all: the
// implied sleep is spread/2 frame periods, so 8 caps it at ~4 (67ms at 60Hz) instead of 46 seconds.
// A legitimate lead is 1, occasionally 2 with frame delay - we run one frame ahead of the display,
// never thousands.
#define GROOVY_RASTER_MAX_SPREAD 8

static bool   bSwitchresUp  = false;
static bool   bHaveModeline = false;
static Modeline curModeline;
static UINT32 nBlitFrame    = 0;

static void ResetClientFrameState()
{
	gm.fpga.frame      = 0;
	gm.fpga.frameEcho  = 0;
	gm.fpga.vCount     = 0;
	gm.fpga.vCountEcho = 0;
	nBlitFrame         = 0;
}

// Session parameters are baked into CMD_INIT and cannot change mid-session: a codec, RGB
// mode, audio rate or MTU change needs a full re-init. Remember what we opened with so we
// can notice and reconnect.
static INT32 nOpenCodec = -1, nOpenRgbMode = -1, nOpenMtu = -1;
static INT32 nOpenSoundRate = -1, nOpenSoundChan = -1;

// Status
static char   szState[96]     = "disabled";
static UINT32 nFramesSent     = 0;
static UINT32 nGateRefusals   = 0;
static UINT32 nAudioBytesSent = 0;
static INT32  nLastSrcW = 0, nLastSrcH = 0, nLastSrcFps = 0, nLastSrcDepth = 0;
static UINT32 nLastBlitBytes  = 0;

// Only nag the user once per distinct refusal, or a refused mode would spam the OSD 60
// times a second.
static GroovyModeResult eLastRefusal = GROOVY_MODE_OK;

// ---------------------------------------------------------------------------
// Frame-cost instrumentation
// ---------------------------------------------------------------------------
//
// Under netplay the answer to "Groovy is eating my frame budget" is to TELL the user, never to
// silently reconfigure mid-match. That makes the measurement the deliverable, so it has to be
// honest: wall-clock, on the emulation thread, separated into the two things that can cost.

static double dPackBlitMs   = 0.0;	// pack + encode + post, inside GroovyFrameReady()
static double dSyncMs       = 0.0;	// total time inside WaitSync, whichever regime

// *** The two halves of dSyncMs, and the reason this instrumentation was rewritten. ***
//
// WaitSync does two completely different jobs depending on who owns the frame clock, and
// charging them to the same counter made the readout meaningless. When WE pace, sleeping out
// the remainder of the frame period IS the job - it is most of a frame by construction, and a
// healthy offline session honestly reported "sync 13.70ms ... 296/299 frames over 50% budget".
// When something else paces (netplay), sleepTime should be 0, so every millisecond spent in
// there is pure overhead - and that is the number that actually says whether Groovy is eating
// a match's frame budget.
static double dPaceSleepMs    = 0.0;	// last frame, when we owned the clock: intentional
static double dSyncOverheadMs = 0.0;	// last frame, when we did not: chargeable

// Charged and cleared by AccumulateFrameCost(), so a frame that never reached WaitSync bills
// nothing rather than re-billing the previous frame's figure.
static double dPendingOverheadMs = 0.0;

static double dWorstFrameMs    = 0.0;	// worst chargeable cost, over a rolling window
static double dWorstOverheadMs = 0.0;	// worst sync overhead alone, same window
static double dOverheadSumMs   = 0.0;	// for the mean over that window
static UINT32 nBudgetMissed = 0;	// frames whose chargeable cost blew the budget fraction
static UINT32 nCostSamples  = 0;
static UINT64 nWorstResetMs = 0;

// A frame is "missed" when our work alone eats this much of the frame period. Not a hard
// failure - emulation, rollback and runahead all still need their share - but past this the
// user should know.
#define GROOVY_BUDGET_FRACTION 0.5

static UINT64 GroovyTickNow()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (UINT64)t.QuadPart;
}

static double GroovyTickMsSince(UINT64 nStart)
{
	static double dTicksPerMs = 0.0;
	if (dTicksPerMs == 0.0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		dTicksPerMs = (double)f.QuadPart / 1000.0;
	}
	return (double)(GroovyTickNow() - nStart) / dTicksPerMs;
}

static void AccumulateFrameCost()
{
	// Only CHARGEABLE time counts against the budget - see dPaceSleepMs / dSyncOverheadMs.
	// Consume the pending overhead so a frame that never reached WaitSync bills nothing
	// instead of re-billing the previous frame's figure.
	const double dOverhead = dPendingOverheadMs;
	dPendingOverheadMs = 0.0;

	const double dTotal = dPackBlitMs + dOverhead;
	if (dTotal > dWorstFrameMs) dWorstFrameMs = dTotal;
	if (dOverhead > dWorstOverheadMs) dWorstOverheadMs = dOverhead;
	dOverheadSumMs += dOverhead;
	nCostSamples++;

	const double dFrameMs = (nAppVirtualFps > 0) ? (100000.0 / (double)nAppVirtualFps) : 16.6667;
	if (dTotal > dFrameMs * GROOVY_BUDGET_FRACTION) nBudgetMissed++;

	// Decay the worst-case every ~5s so a single startup hitch does not sit in the readout
	// forever, and report sustained trouble to the OSD - the only surface visible during a
	// match, since the settings dialog pauses the game.
	const UINT64 nNowMs = (UINT64)timeGetTime();
	if (nWorstResetMs == 0) nWorstResetMs = nNowMs;
	if (nNowMs - nWorstResetMs >= 5000) {
		// Drained every window regardless of kNetGame, so a count cannot carry over from a
		// previous netplay session into an offline one.
		const UINT32 nIdleCalls = QuarkTakeIdleCount();

		if (nCostSamples > 0) {
			const double dMissPct  = 100.0 * (double)nBudgetMissed / (double)nCostSamples;
			const double dMeanOver = dOverheadSumMs / (double)nCostSamples;

			// "sync overhead" is the netplay-relevant figure and should sit near zero during a
			// match; "pacing sleep" is reported for context and is expected to be most of a
			// frame whenever we own the clock.
			GroovyLog(GROOVY_LOG_INFO,
			          "frame cost [%s]: packblit %.2fms, sync overhead %.2fms (mean %.2f worst %.2f), "
			          "pacing sleep %.2fms, worst chargeable %.2fms, %u/%u frames over %.0f%% budget",
			          GroovyPacingActive() ? "groovy-paced" : "external clock",
			          dPackBlitMs, dSyncOverheadMs, dMeanOver, dWorstOverheadMs,
			          dPaceSleepMs, dWorstFrameMs, nBudgetMissed, nCostSamples,
			          GROOVY_BUDGET_FRACTION * 100.0);

			// @groovy diagnostic: is the sync overhead above actually costing netplay anything?
			//
			// The model to test is that the spin is simply the COMPLEMENT of this frame loop's own
			// work - so it should be large exactly when GGPO is NOT the bottleneck. That predicts
			// high overhead alongside a HIGH idle count (plenty of slack) and FEW rollbacks, and
			// the reverse when the peer is struggling. Measured on hardware, two sessions on an
			// identical modeline gave mean 0.23ms and mean 9.62ms; the fast one was the session
			// full of GGPO stalls and frameskips.
			//
			// If that holds, the spin only ever consumes time nothing else wanted and can be left
			// alone. If it does not, the model is wrong and this needs revisiting.
			if (kNetGame) {
				GroovyLog(GROOVY_LOG_INFO,
				          "netplay load: %u ggpo_idle calls over %u frames (%.1f/frame), "
				          "rollbacks %d (%d frames)",
				          nIdleCalls, nCostSamples, (double)nIdleCalls / (double)nCostSamples,
				          nRollbackCount, nRollbackFrames);
			}

			if (dMissPct >= 20.0) {
				TCHAR szMsg[128];
				_sntprintf(szMsg, 127, _T("Groovy: using %.1fms/frame - consider LZ4 or RGB565"),
				           dWorstFrameMs);
				szMsg[127] = _T('\0');
				VidSNewShortMsg(szMsg, 0xFFBF3F, 4000, 0);
			}
		}
		dWorstFrameMs    = 0.0;
		dWorstOverheadMs = 0.0;
		dOverheadSumMs   = 0.0;
		nBudgetMissed = 0;
		nCostSamples  = 0;
		nWorstResetMs = nNowMs;
	}
}

// The raster line each blit syncs to.
//
// vCountSync == 0 means "automatic frame delay", and that calculation
// (groovymister.cpp:962-976) is built entirely on m_emulationTime - which only WaitSync()
// writes, and which measures wall time BETWEEN WaitSync calls. The moment something else owns
// the frame clock that interval becomes the whole frame period, the calculation degenerates to
// vSync = 1 permanently, and the "automatic" part is a fiction. So under an external clock we
// pick the line explicitly rather than pretending.
static uint16_t EffectiveVCountSync()
{
	if (kNetGame && nGroovyVCountSync == 0) {
		return 1;						// raster-chase from the top
	}
	return (uint16_t)nGroovyVCountSync;
}

static void SetState(const char* pszFmt, ...)
{
	va_list args;
	va_start(args, pszFmt);
	vsnprintf(szState, sizeof(szState) - 1, pszFmt, args);
	va_end(args);
	szState[sizeof(szState) - 1] = '\0';
}

// The Groovy client's log sink. Installed before CmdInit - FBNeo is a GUI app, so without
// this a failed handshake is completely invisible.
static void GroovyClientLogSink(const char* pszMsg)
{
	GroovyLogRaw(GROOVY_LOG_ERROR, pszMsg);
}

// ---------------------------------------------------------------------------
// Audio rate mapping
// ---------------------------------------------------------------------------

// CMD_INIT accepts only these rates, and bakes the choice into the session.
static bool GroovyAudioRateOkay(INT32 nRate)
{
	return nRate == 22050 || nRate == 44100 || nRate == 48000;
}

// Connect back-off. See the call site in GroovyFrameReady() for why this matters.
#define GROOVY_RETRY_INTERVAL_MS 5000

static UINT32 nLastConnectFailMs = 0;
static bool   bConnectGaveUp     = false;	// netplay: one failure and we stand down for good

static bool SessionRetryDue()
{
	if (bConnectGaveUp) return false;
	if (nLastConnectFailMs == 0) return true;			// never failed yet

	const UINT32 nNow = (UINT32)timeGetTime();
	return (nNow - nLastConnectFailMs) >= GROOVY_RETRY_INTERVAL_MS;
}

static void NoteConnectFailed()
{
	nLastConnectFailMs = (UINT32)timeGetTime();
	if (nLastConnectFailMs == 0) nLastConnectFailMs = 1;	// 0 means "never"

	if (kNetGame) {
		bConnectGaveUp = true;
		GroovyLog(GROOVY_LOG_ERROR,
		          "connect failed during netplay - standing down for this session rather than "
		          "retrying inside the frame loop");
	}
}


// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

static void SessionClose(const char* pszWhy)
{
	if (bSessionOpen) {
		// CmdClose must come from the thread that owns the socket. We are single-threaded,
		// so that is always true here.
		gm.CmdClose();
		GroovyLog(GROOVY_LOG_ERROR, "session closed (%s)", pszWhy ? pszWhy : "");
	}
	bSessionOpen  = false;
	bStreaming    = false;
	bHaveModeline = false;
	nBlitFrame    = 0;
	nOpenCodec = nOpenRgbMode = nOpenMtu = -1;
	nOpenSoundRate = nOpenSoundChan = -1;
	eLastRefusal  = GROOVY_MODE_OK;
}

// Bring the modeline calculator up.
//
// This MUST happen before the first GroovySwitchresResolve(), not inside SessionOpen():
// resolve answers UNREADY while switchres is down, and UNREADY is a "retry later" that does
// not open a session - so initialising it inside SessionOpen deadlocks and nothing ever
// streams. Idempotent, so calling it every frame costs a bool test.
static bool EnsureSwitchres()
{
	if (bSwitchresUp) return true;

	GroovyLogSetLevel(nGroovyLogLevel);

	if (!GroovySwitchresInit()) {
		SetState("switchres init failed");
		return false;
	}
	bSwitchresUp = true;
	return true;
}

static bool SessionOpen()
{
	if (bSessionOpen) return true;

	GroovyLogSetLevel(nGroovyLogLevel);

	if (!EnsureSwitchres()) return false;

	// NLC does not round-trip RGB565.
	//
	// Verified on the loopback rig with NO FBNeo code in the path - a known 320x240 pattern
	// encoded by the vendored client and decoded by soft_groovy comes back with ~99.6% of
	// bytes wrong and full-scale per-channel deltas. LZ4+RGB565, NLC+RGB888 and LZ4+RGB888
	// are all byte-perfect, so this is specific to the NLC 565 path and is upstream, not
	// ours. Left unguarded it is a silent garbled picture - the same failure class as Rice
	// against a pre-rice core, except this one we CAN detect.
	//
	// LZ4 carries RGB565 losslessly, so honour the user's bandwidth choice and switch the
	// codec rather than refusing to stream. Say so loudly: never substitute silently.
	INT32 nCodec = nGroovyCodec;
	if (nCodec == GROOVY_CODEC_NLC && nGroovyRgbMode == RGB_565) {
		nCodec = GROOVY_CODEC_LZ4;
		GroovyLog(GROOVY_LOG_ERROR,
		          "NLC cannot carry RGB565 (upstream codec limitation) - using LZ4 instead, "
		          "which is lossless at 565. Choose RGB888 to use NLC.");
		VidSNewShortMsg(_T("Groovy: NLC+RGB565 unsupported, using LZ4"), 0xFFBF3F, 5000, 0);
	}

	// Everything below must precede CmdInit.
	gm_set_log_sink(GroovyClientLogSink);

	// The client's verbose level 2 is PER FRAME by design - it emits an "echo"/"ACK" pair for
	// every blit (groovymister.cpp DiffTimeRaster/getACK). A 4271-line file from one session
	// showed what that does to a log meant to be event-driven. Cap it at 1 whenever the file
	// sink is on: level 1 still gives the ~2s RIO telemetry summary, which is the part with
	// diagnostic value, and the debugger output is unaffected.
	INT32 nClientVerbose = nGroovyLogLevel;
	if (bGroovyLogToFile && nClientVerbose > 1) {
		nClientVerbose = 1;
		GroovyLog(GROOVY_LOG_ERROR,
		          "client verbosity capped at 1 for the file log (level 2 is per-frame); "
		          "turn file logging off to use level 2");
	}
	gm.setVerbose((uint8_t)nClientVerbose);

	if (bGroovyUseInputs) {
		gm.BindInputs(_TtoA(szGroovyHost), (uint16_t)nGroovyInputPort);
		gm.setInputCaps(GM_CAP_INPUTS_V2);
	}

	if (nCodec == GROOVY_CODEC_NLC) {
		gm.setNlcPack((uint8_t)nGroovyNlcPack);
		gm.setNearLevel((uint8_t)nGroovyNearLevel);
	}
	gm.setAutoReconnect((uint8_t)(bGroovyAutoReconnect ? 1 : 0));

	// Audio is fixed for the life of the session.
	INT32 nSoundRate = 0, nSoundChan = 0;
	if (nGroovyAudioMode != GROOVY_AUDIO_OFF && bAudOkay && nBurnSoundRate > 0) {
		if (GroovyAudioRateOkay(nBurnSoundRate)) {
			nSoundRate = nBurnSoundRate;
			nSoundChan = 2;					// FBNeo always produces interleaved stereo
		} else {
			GroovyLog(GROOVY_LOG_ERROR,
			          "audio disabled: %dHz is not one of 22050/44100/48000, and the rate is "
			          "baked into CMD_INIT", (int)nBurnSoundRate);
		}
	}

	GroovyLog(GROOVY_LOG_ERROR, "connecting to %s:%d (codec %d, rgb %d, mtu %d, audio %d/%d)",
	          _TtoA(szGroovyHost), (int)nGroovyPort, (int)nCodec, (int)nGroovyRgbMode,
	          (int)nGroovyMtu, (int)nSoundRate, (int)nSoundChan);

	const int nRet = gm.CmdInit(_TtoA(szGroovyHost), (uint16_t)nGroovyPort,
	                            (int)nCodec, (uint32_t)nSoundRate, (uint8_t)nSoundChan,
	                            (uint8_t)nGroovyRgbMode, (uint16_t)nGroovyMtu);
	if (nRet != 0) {
		SetState("no ACK from %s - wrong IP, core not running, firewall, or MTU",
		         _TtoA(szGroovyHost));
		GroovyLog(GROOVY_LOG_ERROR, "CmdInit failed (%d): %s", nRet, szState);
		// CmdClose is idempotent and safe even when CmdInit never succeeded; calling it
		// keeps the inputs socket tidy.
		gm.CmdClose();
		NoteConnectFailed();
		return false;
	}

	bSessionOpen   = true;
	bEverConnected = true;
	bRawCloseSent  = false;		// this session gets its own close
	nLastConnectFailMs = 0;

	// Fresh liveness state, and re-arm the keepalive clock so a slow CmdInit does not look like
	// 2s of silence the moment we connect.
	bSessionDead    = false;
	nLastEchoSeen   = 0;
	nNoEchoBlits    = 0;
	nBadEchos       = 0;
	nLastReconEpoch = gm.reconnectEpoch();
	nLastWireMs     = (UINT32)timeGetTime();

	// *** Load-bearing: CmdInit does NOT do this for us. ***
	// Without it the dead session's fpga.frame comes straight back through the resync in
	// GroovyFrameReady(), we blit a five-thousand-frame-old counter into a core that restarted at
	// zero, and WaitSync spins for ~46s. See ResetClientFrameState().
	ResetClientFrameState();

	// Remember where we connected, so the close cannot be aimed somewhere else (F2).
	strncpy(szOpenHost, _TtoA(szGroovyHost), sizeof(szOpenHost) - 1);
	szOpenHost[sizeof(szOpenHost) - 1] = '\0';
	nOpenPort = nGroovyPort;
	bHaveModeline  = false;
	nBlitFrame     = 0;
	nOpenCodec     = nCodec;	// the codec we ACTUALLY opened with, after the 565 substitution
	nOpenRgbMode   = nGroovyRgbMode;
	nOpenMtu       = nGroovyMtu;
	nOpenSoundRate = nSoundRate;
	nOpenSoundChan = nSoundChan;

	if (bGroovyUseInputs) {
		gm.ResendInputSubscribe();		// UDP-loss insurance; also what fixes reconnects
		GroovyLog(GROOVY_LOG_ERROR, "inputs bound, negotiated caps 0x%02X", gm.getInputCaps());
	}

	// Unconditional (F3): one bounded line per session. A log that shows an open with no matching
	// close is then the smoking gun, readable at a glance, without the user having had to switch
	// file logging on beforehand.
	GroovyLogAlways("session OPEN -> %s:%d (codec=%d rgb=%d) [%s]",
	                szOpenHost, (int)nOpenPort, (int)nCodec, (int)nGroovyRgbMode,
	                GroovyBuildStamp());


	// Record the netplay context once, at the top of the log, so a support question about a
	// match can be answered without guessing at how the session was configured.
	if (kNetGame) {
		GroovyLog(GROOVY_LOG_ERROR,
		          "netplay session: %s, %.2fHz, runahead %d, vCountSync %d (GGPO owns the frame clock)",
		          kNetSpectator ? "spectator/replay" : "player",
		          nAppVirtualFps / 100.0, (int)nVidRunahead, (int)EffectiveVCountSync());

		if (nGroovyVCountSync == 0) {
			GroovyLog(GROOVY_LOG_ERROR,
			          "automatic frame delay is not meaningful under an external clock - "
			          "using raster line 1. Set a explicit sync line to override.");
		}

		// Informational only. Never override the user's latency choices mid-match.
		if (nVidRunahead >= 2) {
			GroovyLog(GROOVY_LOG_ERROR,
			          "note: runahead %d costs an extra emulated frame plus a full state "
			          "save/load per displayed frame, on the same thread as the encode. If the "
			          "frame cost readout looks high, this and the codec are the two knobs.",
			          (int)nVidRunahead);
		}
	}

	SetState("connected");
	return true;
}

// ---------------------------------------------------------------------------
// Raw one-byte commands, sent on a socket of our own
// ---------------------------------------------------------------------------

#ifdef _WIN32
 #define GROOVY_SOCK         SOCKET
 #define GROOVY_SOCK_INVALID INVALID_SOCKET
 #define GroovySockClose(s)  ::closesocket(s)
 #define GroovySockError()   ((int)::WSAGetLastError())
#else
 #define GROOVY_SOCK         int
 #define GROOVY_SOCK_INVALID (-1)
 #define GroovySockClose(s)  ::close(s)
 #define GroovySockError()   (errno)
#endif

#define GROOVY_CMD_CLOSE      1		// handoff Appendix A
#define GROOVY_CMD_GET_STATUS 5

// Send a 1-byte command on a fresh socket, never the client's. Returns 1 if the stack accepted it,
// 0 if the send failed, -1 if we could not even open a socket. pszWhat only labels the log line.
//
// *** The Groovy client cannot do this for us on Windows. ***
//
// USE_RIO is hardcoded to 1 (groovymister.cpp:37), so CmdInit builds m_sockFD with
// WSASocket(..., WSA_FLAG_REGISTERED_IO) and then ::connect()s it (:516, :650). CmdSendClose()
// (:388) calls plain sendto() on that socket and DISCARDS the return value. Winsock documents
// WSAEISCONN for sendto on a connected socket, and RIO sockets are not supported alongside the
// standard Winsock I/O calls - so on Windows it fails, silently, every time. It is correct on
// POSIX, where the same socket is plain and unconnected, which is exactly why the loopback rig
// could never have caught it.
//
// The same reasoning applies to the keepalive, plus one of its own: the upstream CmdSendKeepAlive()
// (absent from our vendored copy) also uses the RIO send path, and a keepalive by definition fires
// when we are NOT blitting - which is exactly when nothing is draining the send completion queue.
// Undrained completions would eventually make RIOSend fail silently. Our own socket has no queue.
//
// A fresh socket sidesteps all of it: not RIO-registered, not connected, nothing being deregistered
// underneath it, and blocking - so sendto returns only once the stack owns the datagram. Winsock
// itself is up for the whole process from main.cpp's WSAStartup, independent of the client's
// refcount, so this still works after teardownVideo() has called WSACleanup(). And the core accepts
// these from any source: process_packet() switches on byte 0 and never looks at the sender.
static int GroovySendRawCmd(const char* pszHost, int nPort, char cCmd, const char* pszWhat)
{
	if (pszHost == NULL || pszHost[0] == '\0') return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons((unsigned short)nPort);
	addr.sin_addr.s_addr = inet_addr(pszHost);
	if (addr.sin_addr.s_addr == INADDR_NONE) {
		GroovyLogAlways("%s: '%s' is not a usable address", pszWhat, pszHost);
		return -1;
	}

	const GROOVY_SOCK s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == GROOVY_SOCK_INVALID) {
		GroovyLogAlways("%s: could not open a socket (error %d)", pszWhat, GroovySockError());
		return -1;
	}

	const int nRet = (int)::sendto(s, &cCmd, 1, 0, (struct sockaddr*)&addr, sizeof(addr));
	if (nRet != 1) {
		GroovyLogAlways("%s: sendto failed (ret %d, error %d)", pszWhat, nRet, GroovySockError());
	}

	GroovySockClose(s);
	return (nRet == 1) ? 1 : 0;
}

// *** Exactly ONE CMD_CLOSE per session. Repeating it is not safe. ***
//
// The obvious hardening for an unacknowledged datagram is to send it two or three times, and
// that is what this code did first. The core cannot take it: setClose() ends with free(poc)
// (groovy.cpp:953) and never NULLs the pointer, and the CMD_CLOSE case is not gated on
// isConnected (:2187). So N consecutive closes with no CMD_INIT between them are N-1 double
// frees inside the MiSTer's groovy server. One datagram, latched, is the correct behaviour -
// and it is what the other working integrations send.
//
// The residual risk we accept: a close landing mid-payload is dispatched through the UDP-loss
// recovery path (:2123), which loses it only when the compressed frame length happens to be
// 1 (mod 1472). That is ~0.07% of frames, against a certainty of corrupting the core's heap.
//
// The latch itself is bRawCloseSent, declared with the other session state at the top.

void GroovySessionClose(const char* pszReason)
{
	// Reached from several exit paths, some of them more than once. Everything below is
	// idempotent, and it must stay FAST: on the MENU_QUIT path this runs inside DrvExit(),
	// which Fightcade executes BEFORE QuarkEnd() -> ggpo_close_session() (scrn.cpp:945-952).
	// Anything slow here delays the match result reaching the server.
	const UINT64 nStart = GroovyTickNow();
	const bool bWasOpen = bSessionOpen;

	// *** Log on the way IN, unconditionally. ***
	//
	// Three rounds of hardware logs could not answer "did the close even run?", because the
	// only trace lived inside the bWasOpen branch - so "ran and found no session" and "never
	// ran at all" produced identical, empty evidence. They need different answers: the first
	// is our bug, the second means the process was killed and nothing in-process can help.
	// GroovyLogAlways, not GroovyLog: this record must survive the user not having switched
	// file logging on. It is bounded - a handful of lines per process run.
	GroovyLogAlways("close: entered (%s) - sessionOpen=%d clientConnected=%d everConnected=%d [%s]",
	                pszReason ? pszReason : "unspecified",
	                (int)bWasOpen, (int)(gm.isConnected() ? 1 : 0), (int)bEverConnected,
	                GroovyBuildStamp());

	// No more frames may re-open this session. Set BEFORE anything below can yield, and before
	// the teardown, because the whole failure mode is a frame slipping in behind us.
	bShutdownRequested = true;

	// Latched: the core would treat extra datagrams as extra closes. See bRawCloseSent.
	//
	// Aimed at szOpenHost/nOpenPort - what SessionOpen() actually connected to - not at the live
	// config, which the settings dialog or a mid-session edit could have moved underneath us.
	if (bEverConnected && !bRawCloseSent) {
		bRawCloseSent = true;
		if (GroovySendRawCmd(szOpenHost, nOpenPort, GROOVY_CMD_CLOSE, "close") > 0) {
			GroovyLogAlways("close: raw CMD_CLOSE accepted by stack -> %s:%d",
			                szOpenHost, (int)nOpenPort);
		} else {
			GroovyLogAlways("close: raw CMD_CLOSE FAILED - the core will hold the last frame");
		}
	}

	// Deliberately NOT calling gm.CmdSendClose() as well: it is a no-op on Windows, which is the
	// whole reason GroovySendRawCmd() exists above.
	//
	// CmdClose(), inside SessionClose(), still runs - we need its local teardown (RIO buffers,
	// socket, WSACleanup) and there is no public entry point that tears down without also posting
	// its own CMD_CLOSE. That post is the one that does not arrive on Windows, so in practice the
	// core sees precisely one close.
	//
	// A duplicate is no longer dangerous in any case: the core now gates CMD_CLOSE on isConnected
	// (groovy.cpp:2210), the fix we asked for in docs/HANDOFF_core_idle_timeout.md. It still keeps
	// poc non-NULL after free() on purpose, because the screensaver/logo path dereferences it.
	SessionClose("requested");
	nLastConnectFailMs = 0;
	bConnectGaveUp     = false;
	if (bSwitchresUp) {
		GroovySwitchresExit();
		bSwitchresUp = false;
	}
	SetState("%s", bGroovyEnabled ? "idle" : "disabled");

	// Timed because it sits on Fightcade's shutdown path. If this is ever more than a
	// millisecond or two, it is delaying the match result and needs looking at.
	GroovyLogAlways("close: done (%s) in %.2fms",
	                pszReason ? pszReason : "unspecified", GroovyTickMsSince(nStart));
}

// Called after every blit. Decides whether the core is still really there; see the block comment
// on nLastEchoSeen for why the plausibility test exists and why "did it change" is not enough.
static void CheckSessionAlive()
{
	if (bSessionDead) return;

	// An internal reconnect resets the core's counters, so treat it as a fresh start rather than
	// reading the discontinuity as death. This is what reconnectEpoch() is for.
	const UINT32 nEpoch = gm.reconnectEpoch();
	if (nEpoch != nLastReconEpoch) {
		GroovyLog(GROOVY_LOG_ERROR, "client reconnected internally (epoch %u) - modeline replayed, "
		          "frame counters reset", nEpoch);
		nLastReconEpoch = nEpoch;
		nLastEchoSeen   = 0;
		nNoEchoBlits    = 0;
		nBadEchos       = 0;

		// The client's internal reconnect never passes through SessionOpen(), so this is the only
		// place that reset happens for it - and it re-inits without clearing fpga.* either.
		ResetClientFrameState();
		return;
	}

	const UINT32 nEcho = gm.fpga.frameEcho;

	// The echo acknowledges a frame we sent, so it may legitimately LAG us (that is just pipeline
	// depth) but can never meaningfully LEAD us. A lead is the fingerprint of the core reading
	// freed memory. Require a few in a row so one odd datagram cannot kill a healthy session.
	if (nEcho > nBlitFrame + GROOVY_ECHO_SLACK) {
		nBadEchos++;
	} else {
		nBadEchos = 0;
		if (nEcho != nLastEchoSeen) {
			nLastEchoSeen = nEcho;
			nNoEchoBlits  = 0;
		} else {
			nNoEchoBlits++;		// not advancing: the core has gone quiet
		}
	}

	const bool bGarbage = (nBadEchos    >= GROOVY_BAD_ECHOS);
	const bool bStale   = (nNoEchoBlits >= GROOVY_DEAD_BLITS);
	if (!bGarbage && !bStale) return;

	bSessionDead = true;
	GroovyLogAlways("session DEAD: %s (echo %u, sent %u) - no further blits until reconnect",
	                bGarbage ? "core returned implausible frame echoes"
	                         : "core stopped acknowledging",
	                nEcho, nBlitFrame);

	// Hand control to the slower back-off. The client's watchdog retries CmdInit every second, and
	// each failed attempt is three blocking getACK(60) calls - up to 180ms on the emulation thread.
	// Unbounded, that stutters a live match once a second for a MiSTer that may be gone for good.
	// (Largely belt-and-braces: CmdClose() clears m_initHost, which the watchdog also requires.)
	gm.setAutoReconnect(0);

	// Closes our side and stops GroovyFrameSync() touching WaitSync, which is what actually keeps
	// the garbage away from the spin loop.
	SessionClose("core stopped responding");

	// Take the normal back-off, or the next frame would see bSessionOpen == false with
	// nLastConnectFailMs == 0 ("never failed, retry now") and re-init immediately - a reconnect
	// storm. This gives 5s offline, and stands down entirely in netplay rather than stuttering a
	// live match, which is the policy the connect path already follows.
	NoteConnectFailed();
}

void GroovyKeepAlive()
{
	// Nothing to hold open, or we deliberately closed it.
	if (!bSessionOpen || bShutdownRequested) return;

	// *** The gate is elapsed WIRE time, not a fixed period. ***
	//
	// nLastWireMs is stamped by every blit and every audio frame, so during emulation this test
	// simply never passes and not one extra packet goes out. The keepalive exists only for the
	// genuinely silent case - a menu, a modal dialog, a pause - where the core would otherwise
	// time us out and free the CRT while we are still very much alive.
	//
	// Callers are expected to poll this FASTER than the threshold (the WM_TIMER in scrn.cpp runs
	// at 250ms). Polling at the threshold itself would let a tick land just under it and defer
	// the send by a whole extra period, roughly doubling the worst-case silence.
	const UINT32 nNow = (UINT32)timeGetTime();
	if ((UINT32)(nNow - nLastWireMs) < GROOVY_KEEPALIVE_IDLE_MS) return;

	nLastWireMs = nNow;		// stamp first: a failed send must not retry every tick

	// CMD_GET_STATUS is the handoff's recommended keepalive - one byte, no side effects, and any
	// datagram resets the core's activity timer.
	GroovySendRawCmd(szOpenHost, nOpenPort, GROOVY_CMD_GET_STATUS, "keepalive");

	// Deliberately not logged per send: at one every 2s an idle session would fill the log. The
	// once-per-transition line below is enough to show the mechanism is alive.
	GroovyLogOnChange(GROOVY_LOG_INFO, GROOVY_LOGKEY_KEEPALIVE,
	                  "keepalive: holding the session open while idle (%d ms threshold)",
	                  GROOVY_KEEPALIVE_IDLE_MS);
}

bool GroovyIsStreaming()
{
	return bStreaming;
}

// Module-internal (groovy_internal.h): what input polling gates on.
bool GroovyInternalIsConnected()
{
	return bSessionOpen;
}

bool GroovyWants32Bit()
{
	// Decided during VidInit(), long before a session exists, so this must not depend on
	// one. RGB565 on the wire is happiest with FBNeo's existing 16bpp render (the pack is
	// then a straight memcpy), so only RGB888 asks for the depth change.
	//
	// *** bDrvOkay is load-bearing, not defensive. ***
	//
	// Netplay calls MediaInit() BEFORE DrvInit() (fbn_ggpo.cpp:246-247), so it performs a
	// full VidInit() with no driver loaded - a splash-sized video init that offline never
	// does at all, because DrvInit deliberately suppresses it by faking bVidOkay
	// (drv.cpp:187-191). Without this guard we changed the render depth during THAT init too,
	// where the two guards below our line in dx9AltTextureInit are themselves `bDrvOkay &&`
	// and therefore dead - silently overriding the user's Force 16-bit setting and driving a
	// code path that has never executed in any configuration, vanilla or otherwise.
	//
	// We only care about the depth once a game is actually streaming, so tie it to that.
	const bool bWants = (bGroovyEnabled != 0) && bDrvOkay && nGroovyRgbMode == RGB_888;

	GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_VIDEOINIT,
	                  "video init: bDrvOkay=%d rgbMode=%d -> render depth %s",
	                  (int)(bDrvOkay ? 1 : 0), (int)nGroovyRgbMode,
	                  bWants ? "32bpp (Groovy)" : "16bpp (stock)");
	return bWants;
}

bool GroovySuppressHostVSync()
{
	// Same bDrvOkay reasoning as GroovyWants32Bit(): never influence the pre-driver VidInit.
	//
	// Netplay is excluded again. Phase 6 removed that term on the argument that a blocking
	// Present() adds jitter to the stream - true, but it was reasoning rather than evidence,
	// and the cost of being wrong is out of proportion: it alters D3D present parameters
	// (COPY vs FLIPEX) with no fallback and, windowed, no error text at all
	// (vid_directx9.cpp:1845-1856). GGPO owns the clock in netplay; leave the user's vsync
	// choice alone there.
	const bool bSuppress = (bGroovyEnabled != 0) && bDrvOkay && !kNetGame;

	GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_VSYNC,
	                  "video init: host vsync suppression %s (bDrvOkay=%d kNetGame=%d)",
	                  bSuppress ? "ON" : "off", (int)(bDrvOkay ? 1 : 0), (int)(kNetGame ? 1 : 0));
	return bSuppress;
}

// A session parameter that is baked into CMD_INIT changed under us.
// The codec actually used, after the NLC+RGB565 substitution in SessionOpen(). Staleness
// must be judged against this, or a substituted session would look stale every frame and
// reconnect forever.
static INT32 EffectiveCodec()
{
	if (nGroovyCodec == GROOVY_CODEC_NLC && nGroovyRgbMode == RGB_565) return GROOVY_CODEC_LZ4;
	return nGroovyCodec;
}

// The CMD_INIT parameters that cannot change mid-session, so a change here means reconnect.
//
// KNOWN GAP: host and port are not checked, and nothing in the settings dialog's apply path closes
// the session either - so changing the MiSTer's IP while streaming does nothing at all, and we
// keep sending to the address we opened with until the session ends by other means. Left as-is
// deliberately; adding them here is the fix, and it is safe now that nothing else depends on a
// live session's endpoint being immovable.
static bool SessionParamsStale()
{
	return bSessionOpen
	    && (nOpenCodec != EffectiveCodec() || nOpenRgbMode != nGroovyRgbMode
	        || nOpenMtu != nGroovyMtu);
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

static void ReportRefusal(GroovyModeResult eResult, const GroovyModeDecision& dec)
{
	if (eResult == eLastRefusal) return;		// already said so; do not spam the OSD
	eLastRefusal = eResult;
	nGateRefusals++;

	switch (eResult) {
		case GROOVY_MODE_NO_MODELINE:
			SetState("no modeline for %dx%d @ %.2fHz on '%s'", (int)nLastSrcW, (int)nLastSrcH,
			         nLastSrcFps / 100.0, GroovySwitchresActivePreset());
			break;
		case GROOVY_MODE_GATED:
			SetState("mode refused: %s", GateResultText(dec.gate));
			break;
		case GROOVY_MODE_SIZE_MISMATCH:
			SetState("mode refused: switchres stretched %dx%d, we do not scale",
			         (int)nLastSrcW, (int)nLastSrcH);
			break;
		default:
			return;
	}

	GroovyLog(GROOVY_LOG_ERROR, "%s", szState);

	// Say it on screen too. A user who is not told reports a refusal as a hang.
	TCHAR szMsg[128];
	_sntprintf(szMsg, 127, _T("Groovy: %s"), _AtoT(szState));
	szMsg[127] = _T('\0');
	VidSNewShortMsg(szMsg, 0xFF3F3F, 5000, 0);
}

void GroovyFrameReady()
{
	// Watch for the edges that legitimately permit a NEW session, before any early return below
	// can hide them from us. Only these two clear bShutdownRequested:
	//
	//   bDrvOkay  0 -> 1   a driver was loaded. DrvInit() always runs DrvExit() first
	//                      (drv.cpp:149), so this is the natural game-to-game boundary.
	//   bGroovyEnabled 0 -> 1   the user re-enabled Groovy in the settings dialog.
	//
	// Anything else - a frame arriving moments after we deliberately closed - must NOT bring the
	// session back. See bShutdownRequested.
	static bool bPrevEnabled = false;
	static bool bPrevDrvOkay = false;
	const bool bNowEnabled = (bGroovyEnabled != 0);
	const bool bNowDrvOkay = (bDrvOkay != 0);
	if ((bNowEnabled && !bPrevEnabled) || (bNowDrvOkay && !bPrevDrvOkay)) {
		bShutdownRequested = false;
	}
	bPrevEnabled = bNowEnabled;
	bPrevDrvOkay = bNowDrvOkay;

	if (!bGroovyEnabled) {
		if (bSessionOpen) GroovySessionClose("disabled by user");
		return;
	}

	// Netplay is supported. GGPO stays the frame clock - see GroovyPacingActive() and
	// GroovyFrameSync() below for how the two coexist.
	if (!bDrvOkay || pVidImage == NULL) return;	// no game: splash and menu redraws are not ours

	// A close has been requested and no new-session edge has been seen since. Frames still arrive
	// here after OnClose/QuarkEnd - PostQuitMessage and bMediaExit both need the message loop to
	// come round - and re-opening on one of those would put the core straight back into a live
	// session that nothing then closes. That is the whole bug this guard exists for.
	if (bShutdownRequested) return;

	const UINT64 nWorkStart = GroovyTickNow();

	// Before the resolve below, not after: see EnsureSwitchres().
	if (!EnsureSwitchres()) return;

	if (SessionParamsStale()) {
		GroovyLog(GROOVY_LOG_ERROR, "session parameters changed - reconnecting");
		SessionClose("params changed");
	}

	// Do not hammer a failed connect.
	//
	// CmdInit makes THREE blocking getACK(60) calls (groovymister.cpp:748/784/806), so a
	// failed attempt costs up to 180ms - and this runs inside VidDoFrame -> VidFrame ->
	// RunFrame, i.e. inside the frame loop GGPO is timing. Retrying every frame would drop
	// the emulator to single-digit fps and starve the peer of inputs. Back off instead, and
	// in netplay give up for the session entirely rather than stutter a live match.
	if (!bSessionOpen && !SessionRetryDue()) return;

	const INT32 nWidth  = nVidImageWidth;
	const INT32 nHeight = nVidImageHeight;
	const INT32 nFps    = nAppVirtualFps;	// post bForce60Hz and post any netplay override

	nLastSrcW = nWidth; nLastSrcH = nHeight; nLastSrcFps = nFps;
	nLastSrcDepth = nVidImageDepth;

	const GroovyModeDecision dec = GroovySwitchresResolve(nWidth, nHeight, nFps);

	if (dec.result != GROOVY_MODE_OK) {
		if (dec.result != GROOVY_MODE_UNREADY) {
			ReportRefusal(dec.result, dec);
			bStreaming = false;
		}
		return;		// keep local play running; retry when the mode next changes
	}

	if (eLastRefusal != GROOVY_MODE_OK) {
		eLastRefusal = GROOVY_MODE_OK;		// recovered
		GroovyLog(GROOVY_LOG_ERROR, "mode accepted again");
	}

	if (!bSessionOpen && !SessionOpen()) return;

	// Send the modeline from this thread, immediately before the first frame that depends
	// on it. Re-sending an identical one costs a core-side mode reset, so only on change.
	if (!bHaveModeline || !curModeline.SameSignal(dec.modeline)) {
		const Modeline& m = dec.modeline;
		gm.CmdSwitchres(m.pclock, m.hActive, m.hBegin, m.hEnd, m.hTotal,
		                m.vActive, m.vBegin, m.vEnd, m.vTotal, m.interlace);
		curModeline   = m;
		bHaveModeline = true;
		GroovyLog(GROOVY_LOG_ERROR, "CmdSwitchres %dx%d %.3fMHz hfreq %.2fkHz interlace %d",
		          m.hActive, m.vActive, m.pclock, m.hfreq / 1000.0, (int)m.interlace);
		SetState("streaming %dx%d @ %.2fkHz", m.hActive, m.vActive, m.hfreq / 1000.0);
	}

	// Never write pixels anywhere but getPBufferBlit(): those buffers are allocated and,
	// on Windows, registered with RIO at CmdInit. A memcpy in is fine; a pointer swap is not.
	char* pBlit = gm.getPBufferBlit(0);
	if (!pBlit) return;

	const bool bFlip = (bGroovyFlip180 != 0)
	                && ((BurnDrvGetFlags() & BDF_ORIENTATION_FLIPPED) != 0);

	const UINT32 nBytes = PackFrame(nVidImageDepth, nGroovyRgbMode,
	                                (const uint8_t*)pVidImage, nVidImagePitch,
	                                nWidth, nHeight, (uint8_t*)pBlit, bFlip);
	if (nBytes == 0) {
		SetState("cannot pack %dbpp source into rgbMode %d", (int)nVidImageDepth, (int)nGroovyRgbMode);
		// On-change only: this condition persists, so a plain log here would be per-frame.
		GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_PACKFAIL, "%s", szState);
		bStreaming = false;
		return;
	}
	nLastBlitBytes = nBytes;

	// The core displays frames in counter order and discards anything behind its current
	// one, so resync if it has moved past us.
	//
	// BOUNDED, and that bound is the whole reconnect-freeze fix. "The core moved past us" is a
	// one- or two-frame condition. A lead of thousands is never a resync - it is the previous
	// session's counter, which CmdInit does not clear (see ResetClientFrameState). Adopting it
	// blitted frame 5473 at a core that had restarted at 1 and spun WaitSync for 46 seconds.
	// H1 already zeroes fpga.frame on every session start; this is the belt to that pair of braces,
	// covering any reconnect path that reaches here before we noticed it.
	if (gm.fpga.frame > nBlitFrame && gm.fpga.frame - nBlitFrame <= GROOVY_RASTER_MAX_SPREAD) {
		nBlitFrame = gm.fpga.frame;
	}
	nBlitFrame++;

	// field is always 0: with interlace byte 2 the core derives the field cadence from the
	// modeline itself. Feeding it an alternating field index would make it read consecutive
	// frames out of its two field buffers and comb on horizontal motion.
	gm.CmdBlit(nBlitFrame, 0, EffectiveVCountSync(), (uint32_t)nGroovyFdMarginNs, 0);
	nLastWireMs = (UINT32)timeGetTime();	// keepalive: a blit IS activity, so it suppresses one

	nFramesSent++;
	bStreaming = true;

	CheckSessionAlive();

	dPackBlitMs = GroovyTickMsSince(nWorkStart);
	AccumulateFrameCost();
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void GroovyAudioFrame(INT16* pPcm, INT32 nSamples)
{
	if (!bSessionOpen || !bStreaming) return;
	if (nGroovyAudioMode == GROOVY_AUDIO_OFF) return;
	if (!pPcm || nSamples <= 0) return;
	if (nOpenSoundChan <= 0) return;			// audio was not negotiated for this session

	// Gate on what the CORE says: it drops audio when its OSD Audio option is off, and this
	// status bit is the only way to find out.
	if (!gm.fpga.audio) return;

	UINT32 nBytes = (UINT32)nSamples * 4u;		// s16 stereo == 4 bytes per sample frame

	// CMD_AUDIO's size field is a u16: a 65536-byte drain casts to 0 and puts an empty
	// CMD_AUDIO on the wire, which the core rejects. Cap well under, on a whole number of
	// stereo frames. Steady state is ~2,940 bytes at 44100/60, so this should never bite.
	const UINT32 nMaxBytes = 16384;
	if (nBytes > nMaxBytes) {
		nBytes = nMaxBytes & ~3u;
		GroovyLogOnChange(GROOVY_LOG_INFO, GROOVY_LOGKEY_AUDIOCLAMP,
		                  "audio drain clamped to %u bytes", nBytes);
	}

	char* pAudio = gm.getPBufferAudio();
	if (!pAudio) return;

	memcpy(pAudio, pPcm, nBytes);
	gm.CmdAudio((uint16_t)nBytes);
	nLastWireMs = (UINT32)timeGetTime();	// audio counts as activity too - any datagram does

	nAudioBytesSent += nBytes;

	// Only now that the audio has actually gone to the MiSTer is it safe to silence the PC.
	// Zeroing rather than skipping AudSoundFrame() keeps the DirectSound play cursor moving,
	// which AudSoundCheck()'s underrun logic reads every idle pass.
	if (nGroovyAudioMode == GROOVY_AUDIO_MISTER) {
		memset(pPcm, 0, (size_t)nSamples * 4u);
	}
}

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------

bool GroovyPacingActive()
{
	// Netplay excluded: GGPO must remain the clock there.
	return bGroovyEnabled && bSessionOpen && bStreaming && !kNetGame;
}

void GroovyWaitSync()
{
	if (!bSessionOpen) return;

	// Decide the regime BEFORE the call: WaitSync itself can change bStreaming's inputs, and
	// the attribution has to describe the call we are about to make.
	const bool bPacing = GroovyPacingActive();

	// *** The backstop. Nothing below this line may spin for seconds. ***
	//
	// WaitSync's sleep is driven by DiffTimeRaster(), which computes
	// dif = ((frameEcho-1)*vTotal + vCountEcho - frame*vTotal - vCount) / 2 and multiplies by
	// m_widthTime. Since m_frameTime = m_widthTime * vTotal, the implied sleep is
	// (m_frameTime * spread) / 2 - half a frame period per frame of divergence. A spread of 5472
	// (the cable-pull reconnect) is 46 SECONDS of busy-spin, and WaitSync accumulates it across
	// iterations, so it never returns. That is a force-kill, and it is what happened on hardware.
	//
	// Only the positive direction spins: a negative dif clamps sleepTime to 0 and falls straight
	// through. So one comparison is enough.
	//
	// H1 and H2 stop the known cause. This stops the CLASS of cause - whatever desynchronises those
	// two counters, we skip pacing for one frame rather than hang the emulator. One frame of
	// imperfect pacing against a 46-second lock is not a difficult trade.
	const UINT32 nEcho = gm.fpga.frameEcho, nCoreFrame = gm.fpga.frame;
	if (nEcho > nCoreFrame && nEcho - nCoreFrame > GROOVY_RASTER_MAX_SPREAD) {
		GroovyLogOnChange(GROOVY_LOG_ERROR, GROOVY_LOGKEY_RASTERSPREAD,
		                  "raster desync: frameEcho %u vs core frame %u (spread %u > %d) - skipping "
		                  "pacing and resetting counters, WaitSync would have slept ~%.1fs",
		                  nEcho, nCoreFrame, nEcho - nCoreFrame, GROOVY_RASTER_MAX_SPREAD,
		                  (double)(nEcho - nCoreFrame) * 0.5 *
		                      ((nAppVirtualFps > 0) ? (100.0 / (double)nAppVirtualFps) : 0.0167));
		ResetClientFrameState();

		// This call cost nothing. Leave dPendingOverheadMs alone - it may hold overhead from
		// earlier in the frame that AccumulateFrameCost() still has to bill.
		dSyncMs         = 0.0;
		dPaceSleepMs    = 0.0;
		dSyncOverheadMs = 0.0;
		return;
	}

	// Called even on frames where nothing was blitted (paused, or a refused mode): on
	// Windows this is the only drain for the RIO send-completion queue.
	const UINT64 nStart = GroovyTickNow();
	gm.WaitSync();
	dSyncMs = GroovyTickMsSince(nStart);

	// Attribute it. When we own the clock this is deliberate sleep and must not be charged as
	// a cost; when something else does, sleepTime should be 0 and all of it is overhead.
	// See AccumulateFrameCost().
	if (bPacing) {
		dPaceSleepMs    = dSyncMs;
		dSyncOverheadMs = 0.0;
	} else {
		dPaceSleepMs    = 0.0;
		dSyncOverheadMs = dSyncMs;
		dPendingOverheadMs += dSyncMs;
	}
}

void GroovyFrameSync()
{
	// The app-master path: something else (GGPO plus FBNeo's timeGetTime accumulator) owns
	// the frame clock, and we are only here to keep the client healthy.
	//
	// We still MUST call WaitSync every frame. drainSendCompletions() is private
	// (groovymister.h:325) and WaitSync (groovymister.cpp:1301) is its only caller in the
	// tree; the send completion queue is 846 entries and a 384x224 RGB888 frame posts ~175
	// sends, so skipping it fills the queue in about five frames, after which RIOSend fails
	// SILENTLY, ACKs stop arriving and the reconnect watchdog thrashes a perfectly good link.
	//
	// It is cheap here precisely because we arrive late. WaitSync computes
	//     sleepTime = (m_emulationTime >= m_frameTime) ? 0 : m_frameTime - m_emulationTime
	// where m_emulationTime is measured from the END of the previous WaitSync. Called once
	// per frame after the host clock has already spent the frame period, sleepTime is 0, the
	// spin loop runs a single iteration and falls straight through to the drain.
	//
	// Called unconditionally on the pass, including frames where no blit went out: a GGPO
	// input stall returns early from RunFrame without producing a frame, and we still want
	// fpga.frameEcho fresh for the status readout.
	// bStreaming, not merely bSessionOpen: before the first CmdSwitchres the client has no
	// modeline, so m_frameTime and m_vTotal are still 0 and the raster servo inside WaitSync
	// has nothing meaningful to work from.
	if (!bSessionOpen || !bStreaming) return;
	if (GroovyPacingActive()) return;	// we own the clock; RunIdle's Groovy branch syncs instead

	GroovyWaitSync();
}

// ---------------------------------------------------------------------------
// Status / self-test
// ---------------------------------------------------------------------------

void GroovyGetStatus(GroovyStatus* pStatus)
{
	if (!pStatus) return;
	memset(pStatus, 0, sizeof(GroovyStatus));

	pStatus->bEnabled   = (bGroovyEnabled != 0);
	pStatus->bConnected = bSessionOpen;
	pStatus->bStreaming = bStreaming;
	strncpy(pStatus->szState, szState, sizeof(pStatus->szState) - 1);

	pStatus->nSrcWidth   = nLastSrcW;
	pStatus->nSrcHeight  = nLastSrcH;
	pStatus->nSrcFpsX100 = nLastSrcFps;
	pStatus->nSrcDepth   = nLastSrcDepth;
	pStatus->nRgbMode    = nGroovyRgbMode;
	pStatus->nBlitBytes  = nLastBlitBytes;

	if (bHaveModeline) {
		pStatus->nModeWidth     = curModeline.hActive;
		pStatus->nModeHeight    = curModeline.vActive;
		pStatus->dHfreqKHz      = curModeline.hfreq / 1000.0;
		pStatus->nInterlaceByte = curModeline.interlace;
	}

	pStatus->nFramesSent     = nFramesSent;
	pStatus->nGateRefusals   = nGateRefusals;
	pStatus->nAudioBytesSent = nAudioBytesSent;

	pStatus->dPackBlitMs      = dPackBlitMs;
	pStatus->dSyncMs         = dSyncMs;
	pStatus->dPaceSleepMs    = dPaceSleepMs;
	pStatus->dSyncOverheadMs = dSyncOverheadMs;
	pStatus->dWorstOverheadMs= dWorstOverheadMs;
	pStatus->dWorstFrameMs   = dWorstFrameMs;
	pStatus->nBudgetMissed = nBudgetMissed;
	pStatus->nCostSamples  = nCostSamples;

	pStatus->bNetGame      = (kNetGame != 0);
	pStatus->bNetSpectator = (kNetSpectator != 0);
	pStatus->nVCountSync   = (INT32)EffectiveVCountSync();

	if (bSessionOpen) {
		pStatus->bCoreAudio   = (gm.fpga.audio != 0);
		pStatus->nFrameskip   = gm.fpga.vgaFrameskip;
		pStatus->bVramSynced  = (gm.fpga.vramSynced != 0);
		pStatus->nInputCaps   = gm.getInputCaps();

		// Raster lag: how far the core's scanout is behind what it last acknowledged.
		const INT32 nLines = (INT32)gm.fpga.vCount - (INT32)gm.fpga.vCountEcho;
		pStatus->nRasterLagLines = nLines;
		if (curModeline.hfreq > 0.0) {
			pStatus->dRasterLagMs = (double)nLines * 1000.0 / curModeline.hfreq;
		}
	}
}

int GroovySelfTest(const char** ppszFirstFail)
{
	return SelfTestPixels(ppszFirstFail);
}
