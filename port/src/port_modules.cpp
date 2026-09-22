// port/src/port_modules: OSLink / OSUnlink over the statically linked REL modules. The game reads
// the REL file from the data tree into its DLL heap and hands the header to OSLink; the port
// ignores the PowerPC image, finds the module by id, gives its .data and .bss the state a fresh
// load would have, and points the header's prolog / epilog / unresolved at the built-in code
// (main_sub.cpp DLL_PROLOG calls through `header->prolog`).
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include "port_modules.h"
#include <dolphin/os.h>
#include <string.h>
#include <stdlib.h>

static PortModule* findModule(u32 id)
{
    for (int i = 0; i < port_module_count; i++) {
        if (port_modules[i].id == id) {
            return (PortModule*) &port_modules[i];
        }
    }
    return NULL;
}

extern "C" {

BOOL OSLink(OSModuleInfo* newModule, void* bss)
{
    PortModule* m = findModule(newModule->id);
    if (m == NULL) {
        port_log("[port] OSLink: no built-in module with id %u\n", (unsigned) newModule->id);
        return FALSE;
    }
    unsigned dataSize = (unsigned) (m->dataEnd - m->dataStart);
    if (m->dataCopy == NULL) {
        m->dataCopy = (unsigned char*) malloc(dataSize ? dataSize : 1);
        memcpy(m->dataCopy, m->dataStart, dataSize);
    } else {
        memcpy(m->dataStart, m->dataCopy, dataSize);
    }
    memset(m->bssStart, 0, (unsigned) (m->bssEnd - m->bssStart));
    OSModuleHeader* h = (OSModuleHeader*) newModule;
    h->prolog = (u32) m->prolog;
    h->epilog = (u32) m->epilog;
    h->unresolved = (u32) m->unresolved;
    port_log("[port] OSLink %s (id %u): data %u bss %u\n", m->name, (unsigned) m->id, dataSize, (unsigned) (m->bssEnd - m->bssStart));
    return TRUE;
}

BOOL OSUnlink(OSModuleInfo* oldModule) { return TRUE; }

}  // extern "C"
