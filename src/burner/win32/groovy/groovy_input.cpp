// Groovy MiSTer - pad input. See groovy_input.h.

#include "burner.h"

#include "groovy_input.h"
#include "groovy_config.h"
#include "groovy_log.h"
#include "groovy_internal.h"		// module-internal: the shared GroovyMister instance

#include <stdio.h>
#include <string.h>

// Analog baselines, so the Input Set dialog does not latch a resting stick. Same threshold
// DirectInput uses over the same -32768..32767 range.
#define GROOVY_AXIS_COUNT 6
#define GROOVY_AXIS_DEADZONE 0x4000
static INT32 nAxisBaseline[GROOVY_JOY_COUNT][GROOVY_AXIS_COUNT] = { { 0 } };

bool GroovyInputActive()
{
	return bGroovyUseInputs != 0 && GroovyInternalIsConnected();
}

bool GroovyInputOwnsJoystick(INT32 nJoystick)
{
	return nJoystick >= GROOVY_JOY_BASE && nJoystick < GROOVY_JOY_BASE + GROOVY_JOY_COUNT;
}

void GroovyInputPoll()
{
	if (!GroovyInputActive()) return;

	// Drains the socket and keeps only the newest packet by (frame, order). Non-blocking,
	// and a no-op before BindInputs.
	gm.PollInputs();
}

// The 32-bit button mask for one pad. v1 cores fill only the low 16 bits, which is all we
// use - bits 12-15 (B9..B12) arrive even on a v1 session.
static UINT32 PadMask(INT32 nPad)
{
	return (nPad == 0) ? gm.joyInputs.joy1 : gm.joyInputs.joy2;
}

INT32 GroovyInputSwitch(INT32 nPad, INT32 nSubCode)
{
	if (!GroovyInputActive()) return 0;
	if (nPad < 0 || nPad >= GROOVY_JOY_COUNT) return 0;

	const UINT32 nMask = PadMask(nPad);

	// D-pad, presented as axis half-directions so the stock default mapper works unchanged.
	switch (nSubCode) {
		case 0x00: return (nMask & GM_JOY_LEFT)  ? 1 : 0;	// X-
		case 0x01: return (nMask & GM_JOY_RIGHT) ? 1 : 0;	// X+
		case 0x02: return (nMask & GM_JOY_UP)    ? 1 : 0;	// Y-
		case 0x03: return (nMask & GM_JOY_DOWN)  ? 1 : 0;	// Y+
	}

	// Right stick as digital half-axes, for anything mapped that way.
	if (nSubCode >= 0x04 && nSubCode <= 0x07) {
		const INT32 nAxis  = 2 + ((nSubCode - 0x04) >> 1);	// 0x04/5 -> RX, 0x06/7 -> RY
		const INT32 nValue = GroovyInputAxis(nPad, nAxis);
		return (nSubCode & 1) ? (nValue >  GROOVY_AXIS_DEADZONE ? 1 : 0)
		                      : (nValue < -GROOVY_AXIS_DEADZONE ? 1 : 0);
	}

	// Buttons 1..12 -> GM_JOY_B1..B12, which are mask bits 4..15.
	if (nSubCode >= 0x80 && nSubCode < 0x80 + 12) {
		const INT32 nButton = nSubCode - 0x80;			// 0-based
		return (nMask & (1u << (4 + nButton))) ? 1 : 0;
	}

	return 0;
}

INT32 GroovyInputAxis(INT32 nPad, INT32 nAxis)
{
	if (!GroovyInputActive()) return 0;
	if (nPad < 0 || nPad >= GROOVY_JOY_COUNT) return 0;

	const fpgaJoyInputs& j = gm.joyInputs;

	// Sticks arrive as signed char. Scale to DirectInput's range asymmetrically so both
	// ends reach full scale: -128 -> -32768 and +127 -> +32767.
	signed char cValue = 0;
	switch (nAxis) {
		case 0: cValue = (nPad == 0) ? j.joy1LXAnalog : j.joy2LXAnalog; break;
		case 1: cValue = (nPad == 0) ? j.joy1LYAnalog : j.joy2LYAnalog; break;
		case 2: cValue = (nPad == 0) ? j.joy1RXAnalog : j.joy2RXAnalog; break;
		case 3: cValue = (nPad == 0) ? j.joy1RYAnalog : j.joy2RYAnalog; break;

		case 4: case 5: {
			// Triggers are uint8 0..255 and only arrive in v2 analog packets. Gate on what
			// was actually NEGOTIATED, never on what we asked for.
			if (!(gm.getInputCaps() & GM_CAP_INPUTS_V2)) return 0;
			UINT8 nTrig;
			if (nAxis == 4) nTrig = (nPad == 0) ? j.joy1LTAnalog : j.joy2LTAnalog;
			else            nTrig = (nPad == 0) ? j.joy1RTAnalog : j.joy2RTAnalog;
			return (INT32)nTrig * 32767 / 255;
		}

		default: return 0;
	}

	return (cValue >= 0) ? ((INT32)cValue * 32767 / 127)
	                     : ((INT32)cValue * 32768 / 128);
}

INT32 GroovyInputFind(bool bCreateBaseline)
{
	INT32 nResult = -1;

	if (GroovyInputActive()) {
		for (INT32 nPad = 0; nPad < GROOVY_JOY_COUNT && nResult < 0; nPad++) {
			const INT32 nJoyCode = 0x4000 | ((GROOVY_JOY_BASE + nPad) << 8);

			// Digital first: d-pad, then buttons.
			for (INT32 i = 0x00; i <= 0x03; i++) {
				if (GroovyInputSwitch(nPad, i)) { nResult = nJoyCode | i; break; }
			}
			if (nResult >= 0) break;

			for (INT32 i = 0x80; i < 0x80 + 12; i++) {
				if (GroovyInputSwitch(nPad, i)) { nResult = nJoyCode | i; break; }
			}
			if (nResult >= 0) break;

			// Analog: only report an axis that has MOVED from its baseline, otherwise a
			// stick resting off-centre (or a trigger at rest) would bind itself instantly.
			for (INT32 nAxis = 0; nAxis < GROOVY_AXIS_COUNT; nAxis++) {
				const INT32 nValue = GroovyInputAxis(nPad, nAxis);
				const INT32 nDelta = nValue - nAxisBaseline[nPad][nAxis];
				if (nDelta < -GROOVY_AXIS_DEADZONE || nDelta > GROOVY_AXIS_DEADZONE) {
					const INT32 nDir = (nDelta > 0) ? 1 : 0;
					nResult = nJoyCode | ((nAxis << 1) | nDir);
					break;
				}
			}
		}
	}

	if (bCreateBaseline) {
		for (INT32 nPad = 0; nPad < GROOVY_JOY_COUNT; nPad++) {
			for (INT32 nAxis = 0; nAxis < GROOVY_AXIS_COUNT; nAxis++) {
				nAxisBaseline[nPad][nAxis] = GroovyInputActive() ? GroovyInputAxis(nPad, nAxis) : 0;
			}
		}
	}

	return nResult;
}

INT32 GroovyInputGetControlName(INT32 nPad, INT32 nSubCode, TCHAR* pszDeviceName, TCHAR* pszControlName)
{
	if (pszDeviceName) {
		_stprintf(pszDeviceName, _T("MiSTer pad %d"), (int)(nPad + 1));
	}

	if (!pszControlName) return 0;
	pszControlName[0] = _T('\0');

	static const TCHAR* const szDirNames[4] = { _T("Left"), _T("Right"), _T("Up"), _T("Down") };
	static const TCHAR* const szAxisNames[GROOVY_AXIS_COUNT] = {
		_T("L-Analog X"), _T("L-Analog Y"), _T("R-Analog X"), _T("R-Analog Y"),
		_T("L-Trigger"),  _T("R-Trigger")
	};

	if (nSubCode >= 0x00 && nSubCode <= 0x03) {
		_stprintf(pszControlName, _T("%s"), szDirNames[nSubCode]);
	} else if (nSubCode >= 0x04 && nSubCode < 0x04 + (GROOVY_AXIS_COUNT - 2) * 2) {
		const INT32 nAxis = 2 + ((nSubCode - 0x04) >> 1);
		_stprintf(pszControlName, _T("%s %s"), szAxisNames[nAxis], (nSubCode & 1) ? _T("+") : _T("-"));
	} else if (nSubCode >= 0x80 && nSubCode < 0x80 + 12) {
		_stprintf(pszControlName, _T("Button %d"), (int)(nSubCode - 0x80 + 1));
	} else {
		_stprintf(pszControlName, _T("Control 0x%02X"), (int)nSubCode);
	}

	return 0;
}
