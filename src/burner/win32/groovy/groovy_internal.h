// Groovy MiSTer - MODULE-INTERNAL header.
//
// Shares the one GroovyMister client instance between the files that make up the module.
//
// *** NEVER include this from outside src/burner/win32/groovy/. *** It pulls in
// groovymister.h and therefore <winsock2.h>, and src/burner/win32/main.cpp includes the
// Winsock 1.1 <winsock.h> - MSVC will not tolerate both in one translation unit. FBNeo-facing
// code uses groovy_output.h / groovy_input.h, which are socket-free by design.
// See src/dep/groovymister/PROVENANCE.md.

#ifndef GROOVY_INTERNAL_H
#define GROOVY_INTERNAL_H

#include "groovymister.h"

// Defined in groovy_output.cpp, which owns the session lifecycle.
extern GroovyMister gm;

// True when a session is up and blits are going out. Input polling gates on this.
bool GroovyInternalIsConnected();

#endif // GROOVY_INTERNAL_H
