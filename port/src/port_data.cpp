// port/src/port_data: SDK data symbols the game objects reference directly.
#include "types.h"
#include "gx.h"

// The GX write-gather pipe as the port's proxy (gx.h): its members call the vertex assembler.
PortWGPipe port_wgpipe;
