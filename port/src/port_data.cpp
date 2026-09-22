// port/src/port_data: SDK data symbols the game objects reference directly.
#include "types.h"
#include "gx.h"

// The GX write-gather pipe: on the GameCube every store to this address pushed a word into the
// GP FIFO. The game writes vertices through it in two units (11 sites); the port's GX layer will
// capture them through a macro. Until then the stores land in this word.
volatile WGPipe GXWGFifo[1];
