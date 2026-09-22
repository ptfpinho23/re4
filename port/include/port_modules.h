#ifndef PORT_MODULES_H
#define PORT_MODULES_H

// The REL modules linked into the executable (tools/port/modlink.py generates the table).
struct PortModule {
    unsigned int id;              // the REL's module id (config/G4BE08/modules/<name>/rel.json)
    const char* name;
    void (*prolog)(void);
    void (*epilog)(void);
    void (*unresolved)(void);
    unsigned char* dataStart;     // the module's .data: restored to its initial bytes on every link
    unsigned char* dataEnd;
    unsigned char* bssStart;      // the module's .bss: zeroed on every link
    unsigned char* bssEnd;
    unsigned char* dataCopy;      // the initial .data, taken at the first link
};

extern const PortModule port_modules[];
extern const int port_module_count;

#endif
