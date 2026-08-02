// MiSTer + DirectInput8 input driver.
//
// A MERGING SHIM, not a replacement. On BUILD_WIN32 the input driver table has exactly one
// entry (INPUT_LEN == 1) and nInputSelect is never persisted, so a second table entry would
// simply be unreachable - and selecting it would cost the user their keyboard. Instead this
// driver delegates every call to the existing DirectInput driver and intercepts only the two
// joystick device indices reserved for the MiSTer's own pads.
//
// With MiSTer input disabled every function below is a straight passthrough, so behaviour is
// bit-for-bit what it was before.

#include "burner.h"
#include "groovy_input.h"

// The real DirectInput driver. We call through its table rather than its statics, so
// inp_dinput.cpp needs no edits at all.
extern struct InputInOut InputInOutDInput;

// Is this a joystick code (0x4000-0x7FFF) belonging to one of our pads?
static inline bool IsGroovyJoyCode(INT32 nCode, INT32* pnPad, INT32* pnSubCode)
{
	if ((nCode & 0xC000) != 0x4000) return false;

	const INT32 nJoystick = (nCode >> 8) & 0x3F;
	if (!GroovyInputOwnsJoystick(nJoystick)) return false;

	if (pnPad)     *pnPad     = nJoystick - GROOVY_JOY_BASE;
	if (pnSubCode) *pnSubCode = nCode & 0xFF;
	return true;
}

static INT32 misterInit()
{
	return InputInOutDInput.Init();
}

static INT32 misterExit()
{
	return InputInOutDInput.Exit();
}

static INT32 misterSetCooperativeLevel(bool bExclusive, bool bForeground)
{
	return InputInOutDInput.SetCooperativeLevel(bExclusive, bForeground);
}

static INT32 misterNewFrame()
{
	// Drain the MiSTer inputs socket once per frame. Non-blocking, and a no-op when MiSTer
	// input is off or no session is up.
	//
	// This runs from InputMake() -> GetInput(true) -> RunFrame, which GGPO skips entirely on
	// rollback frames (bInput == 0), so it is rollback-correct by construction - and it is
	// upstream of NetworkGetInput(), which is where local sampling has to happen.
	GroovyInputPoll();

	return InputInOutDInput.NewFrame();
}

static INT32 misterReadSwitch(INT32 nCode)
{
	INT32 nPad, nSubCode;
	if (IsGroovyJoyCode(nCode, &nPad, &nSubCode)) {
		return GroovyInputSwitch(nPad, nSubCode);
	}
	return InputInOutDInput.ReadSwitch(nCode);
}

static INT32 misterReadJoyAxis(INT32 i, INT32 nAxis)
{
	if (GroovyInputOwnsJoystick(i)) {
		return GroovyInputAxis(i - GROOVY_JOY_BASE, nAxis);
	}
	return InputInOutDInput.ReadJoyAxis(i, nAxis);
}

static INT32 misterReadMouseAxis(INT32 i, INT32 nAxis)
{
	return InputInOutDInput.ReadMouseAxis(i, nAxis);
}

static INT32 misterFind(bool bCreateBaseline)
{
	// Both are called every time, even once one has found something: each maintains its own
	// analog baseline, and letting one go stale would make a resting stick bind itself the
	// next time the dialog opens.
	//
	// DirectInput wins ties, so the existing keyboard/pad/mouse detection order is exactly
	// as it was and the MiSTer is purely additive.
	const INT32 nDInput = InputInOutDInput.Find(bCreateBaseline);
	const INT32 nGroovy = GroovyInputFind(bCreateBaseline);

	return (nDInput >= 0) ? nDInput : nGroovy;
}

static INT32 misterGetControlName(INT32 nCode, TCHAR* pszDeviceName, TCHAR* pszControlName)
{
	INT32 nPad, nSubCode;
	if (IsGroovyJoyCode(nCode, &nPad, &nSubCode)) {
		return GroovyInputGetControlName(nPad, nSubCode, pszDeviceName, pszControlName);
	}
	return InputInOutDInput.GetControlName(nCode, pszDeviceName, pszControlName);
}

struct InputInOut InputInOutMister = {
	misterInit,
	misterExit,
	misterSetCooperativeLevel,
	misterNewFrame,
	misterReadSwitch,
	misterReadJoyAxis,
	misterReadMouseAxis,
	misterFind,
	misterGetControlName,
	NULL,
	_T("DirectInput8 + MiSTer input")
};
