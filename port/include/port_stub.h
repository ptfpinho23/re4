#ifndef PORT_STUB_H
#define PORT_STUB_H

// Unimplemented platform entry points report themselves once (port/src/port_log.cpp) and return
// zero. The generated weak stubs (tools/port/gen_stubs.py) and hand-written placeholders use it.
#ifdef __cplusplus
extern "C" {
#endif
void port_stub_report(const char* name, int* once);
#ifdef __cplusplus
}
#endif

#define PORT_STUB(name)                       \
    do {                                      \
        static int once_ = 0;                 \
        if (!once_) port_stub_report(name, &once_); \
    } while (0)

#endif
