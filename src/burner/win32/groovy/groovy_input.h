// Groovy MiSTer - pads, presented to FBNeo as ordinary joystick devices.
//
// The MiSTer's own controllers are streamed to us over UDP :32101, so a cabinet whose stick
// is wired to the MiSTer needs no PC-side controller at all. Note this is a CONVENIENCE, not
// a latency win - it adds a network hop compared with a stick plugged into the PC - which is
// why it is off by default.
//
// Presented at FBNeo joystick device indices 8 and 9, i.e. input codes 0x4800 and 0x4900:
//   - above MAX_GAMEPAD (8) so they can never collide with a real DirectInput pad;
//   - <= 15 so they round-trip the Input Set dialog, which masks the device with 0x0F00.
//
// The sub-code layout deliberately mirrors DirectInput's exactly, which is what makes this
// nearly free: InputCodeDesc() renders sensible names, config files persist the raw codes
// (`switch 0x4880`), and GamcPlayer(pgi, szi, player, 8) produces a whole default mapping
// with no new mapping code anywhere.
//
//   0x00 / 0x01   X- / X+      (d-pad left / right)
//   0x02 / 0x03   Y- / Y+      (d-pad up / down)
//   0x04..0x07    Z-/Z+, rX-/rX+  (right stick, when analog)
//   0x80..0x8B    buttons 1-12 (GM_JOY_B1..B12)
//
// Include AFTER burner.h. Socket-free: safe from any FBNeo translation unit.

#ifndef GROOVY_INPUT_H
#define GROOVY_INPUT_H

#define GROOVY_JOY_BASE   8		// first FBNeo joystick index we occupy
#define GROOVY_JOY_COUNT  2		// the protocol carries exactly two pads

// True when MiSTer input is enabled, the session is up, and the inputs socket is bound.
bool GroovyInputActive();

// Is this FBNeo joystick index one of ours?
bool GroovyInputOwnsJoystick(INT32 nJoystick);

// Drain the inputs socket. Non-blocking. Called once per frame from the input driver's
// NewFrame(), i.e. from InputMake() -> GetInput(true) -> RunFrame - which GGPO skips
// entirely on rollback frames, so this is rollback-correct by construction, and it sits
// upstream of NetworkGetInput() where it has to be.
void GroovyInputPoll();

// Digital read. nPad is 0-based (nJoystick - GROOVY_JOY_BASE), nSubCode as above.
INT32 GroovyInputSwitch(INT32 nPad, INT32 nSubCode);

// Analog read, scaled to DirectInput's -32768..+32767 range (FBNeo sets that via
// DIPROP_RANGE, and the rest of the input layer assumes it).
// nAxis: 0=LX 1=LY 2=RX 3=RY, 4=LT 5=RT (triggers need negotiated v2 caps).
INT32 GroovyInputAxis(INT32 nPad, INT32 nAxis);

// For the Input Set dialog. Returns a full FBNeo input code, or -1 if nothing is pressed.
// bCreateBaseline latches current analog positions so a resting stick does not register.
INT32 GroovyInputFind(bool bCreateBaseline);

// Names for the Input Set / Input Editor dialogs.
INT32 GroovyInputGetControlName(INT32 nPad, INT32 nSubCode, TCHAR* pszDeviceName, TCHAR* pszControlName);

#endif // GROOVY_INPUT_H
