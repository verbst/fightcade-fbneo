#include "burner.h"
#include "groovy_log.h"	// @groovy diagnostic breadcrumbs (logging only - no behaviour change)
#include "ggponet.h"
#include "ggpoclient.h"
#include "ggpo_perfmon.h"

GGPOSession *ggpo = NULL;
bool bSkipPerfmonUpdates = false;

void QuarkInitPerfMon();
void QuarkPerfMonUpdate(GGPONetworkStats *stats);

extern int nAcbVersion;
extern int nAcbLoadState;
extern int bMediaExit;

// rollback counter
int nRollbackFrames = 0;
int nRollbackCount = 0;

static char pGameName[MAX_PATH];
static bool bDelayLoad = false;
static bool bDirect = false;
static bool bReplaySupport = false;
static bool bReplayStarted = false;
static bool bReplayRecord = false;
static bool bReplayRecording = false;
static int iRanked = 0;
static int iPlayer = 0;
static int iDelay = 0;
static int iSeed = 0;

const int ggpo_state_header_size = 6 * sizeof(int);

int GetHash(const char *id, int len)
{
	unsigned int hash = 1315423911;
	for (int i = 0; i < len; i++) {
		hash ^= ((hash << 5) + id[i] + (hash >> 2));
	}
	return (hash & 0x7FFFFFFF);
}

void SetBurnFPS(const char *name, int version)
{
	if (kNetVersion < NET_VERSION_60FPS) {
		// Version 1: use old framerate (59.94fps)
		bForce60Hz = 1;
		nBurnFPS = 5994;
		nAppVirtualFps = nBurnFPS;
		return;
	}

	// UMK3UC at 60fps rate
	if (kNetVersion >= NET_VERSION_UMK3UC_FRAMERATE) {
		if (!strcmp(name, "umk3uc")) {
			bForce60Hz = 1;
			nBurnFPS = 6000;
			nAppVirtualFps = nBurnFPS;
			return;
		}
	}

	if (kNetVersion < NET_VERSION_DISABLE_FORCE_60HZ) {
		bForce60Hz = 1;
		// MK Framerate at original rate (54fps)
		if (kNetVersion >= NET_VERSION_MK_FRAMERATE) {
			if (!strcmp(name, "mk") || !strcmp(name, "mk2") || !strcmp(name, "mk2p") || !strcmp(name, "mk3") ||
				!strcmp(name, "umk3") || !strcmp(name, "umk3p") || !strcmp(name, "umk3uc") || !strcmp(name, "umk3uk") ||
				!strcmp(name, "wwfman")) {
				bForce60Hz = 0;
				return;
			}
		}
		nBurnFPS = 6000;
		nAppVirtualFps = nBurnFPS;
	}
}

bool __cdecl ggpo_on_client_event_callback(GGPOClientEvent *info)
{
	switch (info->code)
	{
	case GGPOCLIENT_EVENTCODE_CONNECTING:
		VidOverlaySetSystemMessage(_T("Connecting..."));
		VidSSetSystemMessage(_T("Connecting..."));
		break;

	case GGPOCLIENT_EVENTCODE_CONNECTED:
		VidOverlaySetSystemMessage(_T("Connected"));
		VidSSetSystemMessage(_T("Connected"));
		break;

	case GGPOCLIENT_EVENTCODE_RETREIVING_MATCHINFO:
		VidOverlaySetSystemMessage(_T("Retrieving Match Info..."));
		VidSSetSystemMessage(_T("Retrieving Match Info..."));
		break;

	case GGPOCLIENT_EVENTCODE_DISCONNECTED:
		VidOverlaySetSystemMessage(_T("Disconnected!"));
		VidSSetSystemMessage(_T("Disconnected!"));
		QuarkFinishReplay();
		break;

	case GGPOCLIENT_EVENTCODE_MATCHINFO: {
		VidOverlaySetSystemMessage(_T(""));
		VidSSetSystemMessage(_T(""));
		if (kNetSpectator) {
			kNetVersion = strlen(info->u.matchinfo.blurb) > 0 ? atoi(info->u.matchinfo.blurb) : NET_VERSION;
			SetBurnFPS(pGameName, kNetVersion);
		}
		TCHAR szUser1[128];
		TCHAR szUser2[128];
		VidOverlaySetGameInfo(ANSIToTCHAR(info->u.matchinfo.p1, szUser1, 128), ANSIToTCHAR(info->u.matchinfo.p2, szUser2, 128), kNetSpectator, iRanked, iPlayer);
		VidSSetGameInfo(ANSIToTCHAR(info->u.matchinfo.p1, szUser1, 128), ANSIToTCHAR(info->u.matchinfo.p2, szUser2, 128), kNetSpectator, iRanked, iPlayer);
		break;
	}

	case GGPOCLIENT_EVENTCODE_SPECTATOR_COUNT_CHANGED:
		VidOverlaySetGameSpectators(info->u.spectator_count_changed.count);
		VidSSetGameSpectators(info->u.spectator_count_changed.count);
		break;

	case GGPOCLIENT_EVENTCODE_CHAT:
		if (strlen(info->u.chat.text) > 0) {
			TCHAR szUser[128];
			TCHAR szText[1024];
			ANSIToTCHAR(info->u.chat.username, szUser, 128);
			ANSIToTCHAR(info->u.chat.text, szText, 1024);
			VidOverlayAddChatLine(szUser, szText);
			TCHAR szTemp[128];
			_sntprintf(szTemp, 128, _T("«%.32hs» "), info->u.chat.username);
			VidSAddChatLine(szTemp, 0XFFA000, ANSIToTCHAR(info->u.chat.text, NULL, 0), 0xEEEEEE);
		}
		break;

	default:
		break;
	}
	return true;
}

bool __cdecl ggpo_on_client_game_callback(GGPOClientEvent *info)
{
	// DEPRECATED
	return true;
}

bool __cdecl ggpo_on_event_callback(GGPOEvent *info)
{
	// @groovy diagnostic: the session lifecycle, logged BEFORE the client-event forward so
	// the Fightcade client codes (5000+: connecting / connected / matchinfo / chat) are
	// visible too. Those are what tell us the SERVER side is fine; the 1000-series codes are
	// the peer-to-peer session. If the 1000s never appear, the match never synchronised and
	// nothing downstream - including Groovy - was ever going to happen.
	GroovyTrace("ggpo: event %d%s", (int)info->code,
	            info->code == GGPO_EVENTCODE_CONNECTED_TO_PEER       ? " CONNECTED_TO_PEER" :
	            info->code == GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER ? " SYNCHRONIZING" :
	            info->code == GGPO_EVENTCODE_RUNNING                 ? " RUNNING" :
	            info->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER  ? " DISCONNECTED" :
	            info->code == GGPO_EVENTCODE_TIMESYNC                ? " TIMESYNC" :
	            info->code >= 5000                                   ? " (fightcade client event)" : "");

	if (ggpo_is_client_eventcode(info->code)) {
		return ggpo_on_client_event_callback((GGPOClientEvent *)info);
	}
	if (ggpo_is_client_gameevent(info->code)) {
		return ggpo_on_client_game_callback((GGPOClientEvent *)info);
	}
	GroovyTrace("ggpo: peer event %d%s", (int)info->code,
	            info->code == GGPO_EVENTCODE_CONNECTED_TO_PEER       ? " CONNECTED_TO_PEER" :
	            info->code == GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER ? " SYNCHRONIZING" :
	            info->code == GGPO_EVENTCODE_RUNNING                 ? " RUNNING" :
	            info->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER  ? " DISCONNECTED" :
	            info->code == GGPO_EVENTCODE_TIMESYNC                ? " TIMESYNC" : "");

	switch (info->code) {
	case GGPO_EVENTCODE_CONNECTED_TO_PEER:
		VidOverlaySetSystemMessage(_T("Connected to Peer"));
		VidSSetSystemMessage(_T("Connected to Peer"));
		break;

	case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
		//_stprintf(status, _T("Synchronizing with Peer (%d/%d)..."), info->u.synchronizing.count, info->u.synchronizing.total);
		VidOverlaySetSystemMessage(_T("Synchronizing with Peer..."));
		VidSSetSystemMessage(_T("Synchronizing with Peer..."));
		break;

	case GGPO_EVENTCODE_RUNNING: {
		VidOverlaySetSystemMessage(_T(""));
		VidSSetSystemMessage(_T(""));
		// send ReceiveVersion message
		char temp[16];
		sprintf(temp, "%d", NET_VERSION);
		QuarkSendChatCmd(temp, 'V');
		break;
	}

	case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
		VidOverlaySetSystemMessage(_T("Disconnected from Peer"));
		VidSSetSystemMessage(_T("Disconnected from Peer"));
		if (bReplayRecording) {
			AviStop();
			bMediaExit = true;
		}
		break;

	case GGPO_EVENTCODE_TIMESYNC:
		break;

	default:
		break;
	}

	return true;
}

bool __cdecl ggpo_begin_game_callback(char *name)
{
	WIN32_FIND_DATA fd;
	TCHAR tfilename[MAX_PATH];
	TCHAR tname[MAX_PATH];

	strcpy(pGameName, name);
	ANSIToTCHAR(name, tname, MAX_PATH);
	SetBurnFPS(name, kNetVersion);

	// @groovy diagnostic. This callback has three exits and they load the driver in totally
	// different ways: a savestate launch goes through BurnStateLoad + DrvInitCallback and
	// never touches MediaInit, while a cold launch does MediaInit() then DrvInit(). Knowing
	// which one ran is the difference between guessing and not.
	GroovyTrace("ggpo: begin_game '%s' spectator=%d ranked=%d fps=%d", name,
	            (int)kNetSpectator, (int)iRanked, (int)nBurnFPS);

	if (!kNetSpectator)
	{
		// ranked savestate
		if (iRanked) {
			_stprintf(tfilename, _T("savestates\\%s_fbneo_ranked.fs"), tname);
			if (FindFirstFile(tfilename, &fd) != INVALID_HANDLE_VALUE) {
				// Load our save-state file (freeplay, event mode, etc.)
				GroovyTrace("ggpo: path=RANKED SAVESTATE, loading state (driver comes up via DrvInitCallback)");
				BurnStateLoad(tfilename, 1, &DrvInitCallback);
				GroovyTrace("ggpo: ranked savestate loaded, bDrvOkay=%d bVidOkay=%d", (int)bDrvOkay, (int)bVidOkay);
				DetectorLoad(name, false, iSeed);
				// if playing a direct game, we never get match information, so put anonymous
				if (bDirect) {
					VidOverlaySetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, iPlayer);
					VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, iPlayer);
				}
				return 0;
			}
		}

		// regular savestate
		_stprintf(tfilename, _T("savestates\\%s_fbneo.fs"), tname);
		if (FindFirstFile(tfilename, &fd) != INVALID_HANDLE_VALUE) {
			// Load our save-state file (freeplay, event mode, etc.)
			GroovyTrace("ggpo: path=SAVESTATE, loading state (driver comes up via DrvInitCallback)");
			BurnStateLoad(tfilename, 1, &DrvInitCallback);
			GroovyTrace("ggpo: savestate loaded, bDrvOkay=%d bVidOkay=%d", (int)bDrvOkay, (int)bVidOkay);
			DetectorLoad(name, false, iSeed);
			// if playing a direct game, we never get match information, so put anonymous
			if (bDirect) {
				VidOverlaySetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, iPlayer);
				VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, iPlayer);
			}
			return 0;
		}
	}

	// no savestate or replay
	UINT32 i;
	for (i = 0; i < nBurnDrvCount; i++) {
		nBurnDrvActive = i;
		if ((_tcscmp(BurnDrvGetText(DRV_NAME), tname) == 0) && (!(BurnDrvGetFlags() & BDF_BOARDROM))) {
			if (!kNetSpectator) {
				GroovyTrace("ggpo: path=COLD LOAD, MediaInit() then DrvInit()");
				MediaInit();
				GroovyTrace("ggpo: MediaInit done, bVidOkay=%d", (int)bVidOkay);
				DrvInit(i, true);
				GroovyTrace("ggpo: DrvInit done, bDrvOkay=%d bVidOkay=%d", (int)bDrvOkay, (int)bVidOkay);
			} else {
				bDelayLoad = true;
			}
			DetectorLoad(name, false, iSeed);
			// if playing a direct game, we never get match information, so play anonymous
			if (bDirect) {
				VidOverlaySetGameInfo(_T("player 1#0,0"), _T("player 2#0,0"), false, iRanked, iPlayer);
				VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, iPlayer);
			}
			return 1;
		}
	}

	return 0;
}

bool __cdecl ggpo_advance_frame_callback(int flags)
{
	bSkipPerfmonUpdates = true;
	nFramesEmulated--;
	nRollbackFrames++;
	RunFrame(0, 0, 0);
	bSkipPerfmonUpdates = false;
	return true;
}

static char gAcbBuffer[16 * 1024 * 1024];
static char *gAcbScanPointer;
static int gAcbChecksum;
static FILE *gAcbLogFp;

void ComputeIncrementalChecksum(struct BurnArea *pba)
{
	/*
	 * Ignore checksums in release builds for now.  It takes a while.
	 */
#if defined(FBA_DEBUG)
	int i;

#if 0
	static char *soundAreas[] = {
	   "Z80",
	   "YM21",
	   "nTicksDone",
	   "nCyclesExtra",
	};
	/*
	 * This is a really crappy checksum routine, but it will do for
	 * our purposes
	 */
	for (i = 0; i < ARRAYSIZE(soundAreas); i++) {
		if (!strncmp(soundAreas[i], pba->szName, strlen(soundAreas[i]))) {
			return;
		}
	}
#endif

	for (i = 0; i < pba->nLen; i++) {
		int b = ((unsigned char *)pba->Data)[i];
		if (b) {
			if (i % 2)
				gAcbChecksum *= b;
			else
				gAcbChecksum += b * 317;
		}
		else
			gAcbChecksum++;
	}
#endif
}

static int QuarkLogAcb(struct BurnArea* pba)
{
	fprintf(gAcbLogFp, "%s:", pba->szName);
	int col = 10, row = 30;
	for (int i = 0; i < (int)pba->nLen; i++) {
		if ((i % row) == 0)
			fprintf(gAcbLogFp, "\noffset %9d :", i);
		else if ((i % col) == 0)
			fprintf(gAcbLogFp, " - ");
		fprintf(gAcbLogFp, " %02x", ((unsigned char*)pba->Data)[i]);
	}
	fprintf(gAcbLogFp, "\n");
	ComputeIncrementalChecksum(pba);
	return 0;
}

static int QuarkReadAcb(struct BurnArea* pba)
{
	//ComputeIncrementalChecksum(pba);
	memcpy(gAcbScanPointer, pba->Data, pba->nLen);
	gAcbScanPointer += pba->nLen;
	return 0;
}
static int QuarkWriteAcb(struct BurnArea* pba)
{
	memcpy(pba->Data, gAcbScanPointer, pba->nLen);
	gAcbScanPointer += pba->nLen;
	return 0;
}

bool __cdecl ggpo_save_game_state_callback(unsigned char **buffer, int *len, int *checksum, int frame)
{
	int payloadsize;

	gAcbChecksum = 0;
	gAcbScanPointer = gAcbBuffer;
	BurnAcb = QuarkReadAcb;
	BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
	payloadsize = gAcbScanPointer - gAcbBuffer;

	*checksum = gAcbChecksum;
	*len = payloadsize + ggpo_state_header_size;
	*buffer = (unsigned char *)malloc(*len);

	int *data = (int *)*buffer;
	data[0] = 'GGPO';
	data[1] = ggpo_state_header_size;
	data[2] = nBurnVer;
	data[3] = 0;
	data[4] = 0;
	data[5] = 0;
	// save game state in ranked match (so spectators can get the actual score)
	if (!kNetSpectator) {
		// 32 bits for state, scores and ranked
		int state, score1, score2, start1, start2;
		DetectorGetState(state, score1, score2, start1, start2);
		data[3] = state | ((score1 & 0xff) << 8) | ((score2 & 0xff) << 16) | (iRanked << 24);
		data[4] = (start1 & 0xff) | ((start2 & 0xff) << 8);
	}
	memcpy((*buffer) + ggpo_state_header_size, gAcbBuffer, payloadsize);
	return false;
}

bool __cdecl ggpo_load_game_state_callback(unsigned char *buffer, int len)
{
	if (bDelayLoad) {
		DrvInit(nBurnDrvActive, true);
		MediaInit();
		RunInit();
		bDelayLoad = false;
	}

	int *data = (int *)buffer;
	if (data[0] == 'GGPO') {
		int headersize = data[1];
		int num = headersize / sizeof(int);
		// version
		nAcbVersion = data[2];
		int state = (data[3]) & 0xff;
		int score1 = (data[3] >> 8) & 0xff;
		int score2 = (data[3] >> 16) & 0xff;
		int ranked = (data[3] >> 24) & 0xff;
		int start1 = 0;
		int start2 = 0;
		if (num > 4) {
			start1 = (data[4]) & 0xff;
			start2 = (data[4] >> 8) & 0xff;
		}
		// if spectating, set ranked flag and score
		if (kNetSpectator) {
			iRanked = ranked;
			DetectorSetState(state, score1, score2, start1, start2);
			VidOverlaySetGameInfo(0, 0, kNetSpectator, iRanked, 0);
			VidOverlaySetGameScores(score1, score2);
			VidSSetGameInfo(0, 0, kNetSpectator, iRanked, 0);
			VidSSetGameScores(score1, score2);
		}
		buffer += headersize;
	}
	gAcbScanPointer = (char *)buffer;
	BurnAcb = QuarkWriteAcb;
	nAcbLoadState = kNetSpectator;
	BurnAreaScan(ACB_FULLSCANL | ACB_WRITE, NULL);
	nAcbLoadState = 0;
	nAcbVersion = nBurnVer;
	nRollbackCount++;
	return true;
}

bool __cdecl ggpo_log_game_state_callback(char *filename, unsigned char *buffer, int len)
{
	/*
	 * Note: this is destructive since it relies on loading game
	 * state before scanning!  Luckily, we only call the logging
	 * routine for fatal errors (we should still fix this, though).
	 */
	ggpo_load_game_state_callback(buffer, len);

	gAcbLogFp = fopen(filename, "w");

	gAcbChecksum = 0;
	BurnAcb = QuarkLogAcb;
	BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
	fprintf(gAcbLogFp, "\n");
	fprintf(gAcbLogFp, "Checksum:       %d\n", gAcbChecksum);
	fprintf(gAcbLogFp, "Buffer Pointer: %p\n", buffer);
	fprintf(gAcbLogFp, "Buffer Len:     %d\n", len);

	fclose(gAcbLogFp);

	return true;
}

void __cdecl ggpo_free_buffer_callback(void *buffer)
{
	free(buffer);
}

// ggpo_set_frame_delay from lib
typedef INT32(_cdecl *f_ggpo_set_frame_delay)(GGPOSession *, int frames);
static f_ggpo_set_frame_delay ggpo_set_frame_delay;

static bool ggpo_init()
{
	// load missing ggpo_set_frame_delay from newer ggponet.dll, not available on ggponet.lib
	HINSTANCE hLib = LoadLibrary(_T("ggponet.dll"));
	if (!hLib) {
		return false;
	}
	ggpo_set_frame_delay = (f_ggpo_set_frame_delay)GetProcAddress(hLib, "ggpo_set_frame_delay");
	if (!ggpo_set_frame_delay) {
		return false;
	}

	FreeLibrary(hLib);
	return true;
}

void QuarkInit(TCHAR *tconnect)
{
	ggpo_init();

	char connect[MAX_PATH];
	TCHARToANSI(tconnect, connect, MAX_PATH);
	char game[128], quarkid[128], server[128];
	int port = 0;
	int delay = 0;
	int ranked = 0;
	int live = 0;
	int frames = 0;
	int player = 0;
	int localPort, remotePort;

	GroovyTrace("ggpo: QuarkInit [%s]", TCHARToANSI(tconnect, NULL, 0));
	kNetVersion = NET_VERSION;
	kNetGame = 1;
	kNetLua = 0;
	kNetSpectator = 0;
	kNetQuarkId[0] = 0;
	bForce60Hz = 0;
	iRanked = 0;
	iPlayer = 0;
	iDelay = 0;

#ifdef _DEBUG
	kNetLua = 1;
#endif

	GGPOSessionCallbacks cb = { 0 };

	cb.begin_game = ggpo_begin_game_callback;
	cb.load_game_state = ggpo_load_game_state_callback;
	cb.save_game_state = ggpo_save_game_state_callback;
	cb.log_game_state = ggpo_log_game_state_callback;
	cb.free_buffer = ggpo_free_buffer_callback;
	cb.advance_frame = ggpo_advance_frame_callback;
	cb.on_event = ggpo_on_event_callback;

	if (strncmp(connect, "quark:served", strlen("quark:served")) == 0) {
		sscanf(connect, "quark:served,%[^,],%[^,],%d,%d,%d", game, quarkid, &port, &delay, &ranked);
		iRanked = ranked;
		iPlayer = atoi(&quarkid[strlen(quarkid) - 1]);
		if (nVidRunahead == 3 && delay < 2) {delay = 2;}
		iDelay = delay;
		iSeed = GetHash(quarkid, strlen(quarkid) - 2);
		ggpo = ggpo_client_connect(&cb, game, quarkid, port);
		ggpo_set_frame_delay(ggpo, delay);
		strcpy(kNetQuarkId, quarkid);
		VidOverlaySetSystemMessage(_T("Connecting..."));
	}
	if (strncmp(connect, "quark:training", strlen("quark:training")) == 0) {
		sscanf(connect, "quark:training,%[^,],%[^,],%d,%d", game, quarkid, &port, &delay);
		iRanked = 0;
		iPlayer = atoi(&quarkid[strlen(quarkid) - 1]);
		if (nVidRunahead == 3 && delay < 2) {delay = 2;}
		iDelay = delay;
		iSeed = GetHash(quarkid, strlen(quarkid) - 2);
		ggpo = ggpo_client_connect(&cb, game, quarkid, port);
		ggpo_set_frame_delay(ggpo, delay);
		strcpy(kNetQuarkId, quarkid);
		VidOverlaySetSystemMessage(_T("Connecting..."));
		kNetLua = 1;
		FBA_LoadLuaCode("fbneo-training-mode/fbneo-training-mode.lua");
	}
	else if (strncmp(connect, "quark:direct", strlen("quark:direct")) == 0) {
		sscanf(connect, "quark:direct,%[^,],%d,%[^,],%d,%d,%d,%d", game, &localPort, server, &remotePort, &player, &delay, &ranked);
		kNetLua = 1;
		bDirect = true;
		iRanked = 0;
		iPlayer = player;
		iDelay = delay;
		iSeed = 0;
		ggpo = ggpo_start_session(&cb, game, localPort, server, remotePort, player);
		ggpo_set_frame_delay(ggpo, delay);
		VidOverlaySetSystemMessage(_T("Connecting..."));
	}
	/*
	else if (strncmp(connect, "quark:synctest", strlen("quark:synctest")) == 0) {
	  sscanf(connect, "quark:synctest,%[^,],%d", game, &frames);
	  ggpo = ggpo_start_synctest(&cb, game, frames);
	}
	*/
	else if (strncmp(connect, "quark:stream", strlen("quark:stream")) == 0) {
		sscanf(connect, "quark:stream,%[^,],%[^,],%d", game, quarkid, &remotePort);
		bVidAutoSwitchFullDisable = true;
		kNetSpectator = 1;
		kNetLua = 1;
		iSeed = 0;
		ggpo = ggpo_start_streaming(&cb, game, quarkid, remotePort);
		strcpy(kNetQuarkId, quarkid);
		VidOverlaySetSystemMessage(_T("Connecting..."));
	}
	else if (strncmp(connect, "quark:replay", strlen("quark:replay")) == 0) {
		bVidAutoSwitchFullDisable = true;
		kNetSpectator = 1;
		kNetLua = 1;
		iSeed = 0;
		ggpo = ggpo_start_replay(&cb, connect + strlen("quark:replay,"));
		strcpy(kNetQuarkId, quarkid);
		VidOverlaySetSystemMessage(_T("Connecting..."));
	}
	else if (strncmp(connect, "quark:debugdetector", strlen("quark:debugdetector")) == 0) {
		sscanf(connect, "quark:debugdetector,%[^,]", game);
		kNetGame = 0;
		iRanked = 1;
		iPlayer = 0;
		iSeed = 0x133;
		kNetLua = 1;
		// load game
		TCHAR tgame[128];
		ANSIToTCHAR(game, tgame, 128);
		for (UINT32 i = 0; i < nBurnDrvCount; i++) {
			nBurnDrvActive = i;
			if ((_tcscmp(BurnDrvGetText(DRV_NAME), tgame) == 0) && (!(BurnDrvGetFlags() & BDF_BOARDROM))) {
				// Load game
				MediaInit();
				DrvInit(i, true);
				// Load our save-state file (freeplay, event mode, etc.)
				WIN32_FIND_DATA fd;
				TCHAR tfilename[MAX_PATH];
				_stprintf(tfilename, _T("savestates\\%s_fbneo.fs"), tgame);
				if (FindFirstFile(tfilename, &fd) != INVALID_HANDLE_VALUE) {
					BurnStateLoad(tfilename, 1, &DrvInitCallback);
				}
				DetectorLoad(game, true, iSeed);
				VidOverlaySetGameInfo(_T("Detector1#0,0,0"), _T("Detector2#0,0,0"), false, iRanked, iPlayer);
				VidOverlaySetGameSpectators(0);
				VidSSetGameInfo(_T("Detector1"), _T("Detector2"), false, iRanked, iPlayer);
				VidSSetGameSpectators(0);
				break;
			}
		}
	}
}

void QuarkEnd()
{
	GroovyTrace("ggpo: QuarkEnd - match over");

	ConfigGameSave(bSaveInputs);

	ggpo_close_session(ggpo);
	kNetGame = 0;
	bMediaExit = true;
}

void QuarkTogglePerfMon()
{
	static bool initialized = false;
	if (!initialized) {
		ggpoutil_perfmon_init(hScrnWnd);
	}
	ggpoutil_perfmon_toggle();
}

// @groovy diagnostic: how often GGPO actually gets idled.
//
// run.cpp:437-451 guarantees ONE call per displayed frame (via nRunQuark) and grants further calls
// only while nAccTime < nFps - so this counter is a direct measure of how much slack the frame loop
// had. It is the other half of the "sync overhead" figure in the Groovy frame-cost line: the
// client's WaitSync busy-spins whatever this loop did not spend, so high spin should coincide with
// a HIGH idle count (plenty of slack, GGPO not the bottleneck) and low spin with a low one.
// Read and reset by GroovyGetGgpoIdleCount().
static UINT32 nGgpoIdleCalls = 0;

UINT32 QuarkTakeIdleCount()
{
	const UINT32 n = nGgpoIdleCalls;
	nGgpoIdleCalls = 0;
	return n;
}

void QuarkRunIdle(int ms)
{
	nGgpoIdleCalls++;
	ggpo_idle(ggpo, ms);

	// @groovy diagnostic: a heartbeat while the session is not producing frames.
	//
	// If ggpo_synchronize_input keeps failing we get no frames, no events and no clue
	// whether the peer is even reachable. Every 5 SECONDS - not per frame - report what GGPO
	// itself thinks: a moving ping or non-zero queues means packets are flowing and the
	// session is simply not synchronising; a dead ping means the peer was never reached.
	static UINT32 nLastBeatMs = 0;
	static UINT32 nLastFrames = 0;
	const UINT32 nNow = (UINT32)timeGetTime();

	if (nLastBeatMs == 0) nLastBeatMs = nNow;
	if ((nNow - nLastBeatMs) >= 5000) {
		if (nFramesEmulated == nLastFrames) {		// nothing advanced in the last 5s
			GGPONetworkStats st;
			memset(&st, 0, sizeof(st));
			ggpo_get_stats(ggpo, &st);
			GroovyTrace("ggpo: NO FRAMES for 5s - ping=%d kbps=%d sendQ=%d recvQ=%d predictQ=%d "
			            "localBehind=%d remoteBehind=%d",
			            st.network.ping, st.network.kbps_sent,
			            st.network.send_queue_len, st.network.recv_queue_len,
			            st.network.predict_queue_len,
			            st.timesync.local_frames_behind, st.timesync.remote_frames_behind);
		}
		nLastFrames = nFramesEmulated;
		nLastBeatMs = nNow;
	}
}

bool QuarkGetInput(void *values, int size, int players)
{
	return ggpo_synchronize_input(ggpo, values, size, players);
}

bool QuarkIncrementFrame()
{
	// start auto replay
	if (bReplayRecord) {
		bReplayRecord = false;
		bReplayRecording = true;
		AviStart();
	}

	ggpo_advance_frame(ggpo);

	if (!bSkipPerfmonUpdates) {
		GGPONetworkStats stats;
		ggpo_get_stats(ggpo, &stats);
		ggpoutil_perfmon_update(ggpo, stats);
	}

	if (!bReplaySupport && !bReplayStarted) {
		bReplayStarted = true;
	}

	return true;
}

void QuarkSendChatText(char *text)
{
	QuarkSendChatCmd(text, 'T');
}

void QuarkSendChatCmd(char *text, char cmd)
{
	char buffer[1024]; // command chat
	buffer[0] = cmd;
	strncpy(&buffer[1], text, 1023);
	ggpo_client_chat(ggpo, buffer);
}

void QuarkUpdateStats(double fps)
{
	GGPONetworkStats stats;
	ggpo_get_stats(ggpo, &stats);
	VidSSetStats(fps, stats.network.ping, iDelay);
	VidOverlaySetStats(fps, stats.network.ping, iDelay);
}

void QuarkRecordReplay()
{
	bReplayRecord = true;
	bReplayRecording = false;
}

void QuarkFinishReplay()
{
	if (!bReplaySupport && bReplayStarted) {
		bReplayStarted = false;
		kNetGame = 0;
		if (bReplayRecording) {
			AviStop();
			bMediaExit = true;
		}
	}
}
