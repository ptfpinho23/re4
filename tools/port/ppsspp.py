#!/usr/bin/env python3
"""Drive the port in PPSSPPHeadless through its WebSocket debugger: boot the EBOOT, wait, press
buttons, take screenshots, read the game's log. Needs the `websockets` package (pip).

    ppsspp.py [-t SECS] [-j] [--log FILE] CMD...      GUI=1 in the environment: PPSSPPSDL instead of headless
      wait N            run for N seconds
      press BTN[,BTN]   press buttons (cross circle square triangle up down left right start
                        select l r) for 0.2 s;  hold BTN N;  down BTN / up BTN
      shot FILE.png     screenshot of the current frame
      analog X Y        left stick (-1..1)
      waitlog RE SECS   run until the port's memory-stick log has a line matching RE
      where             every thread's PC as a function (build/port/re4.nm)
      peek SYM[+off] N  hex dump N bytes at a symbol (or hex address)
      poke SYM[+off] HEX|text   write bytes;  break ADDR / unbreak ADDR / watch ADDR N / waitbreak SECS / regs / cont
      quit              stop the emulator (implied at the end)
    -t: PPSSPPHeadless --timeout in emulated seconds (default 100000; the driver kills the emulator anyway)   -j: JIT instead of the interpreter
The emulator's log goes to --log (default build/port/headless.log): the game's own output, or
with FULLLOG=1 every HLE call too (it floods the debugger socket: expect dropped connections in
heavy scenes); the game's lines and faults are printed at the end, de-duplicated (RAW=1: all).
"""
import asyncio, base64, json, os, re, subprocess, sys, time
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parents[2]
EBOOT = Path.home() / ".ppsspp/PSP/GAME/RE4/EBOOT.PBP"
PORT = 45678

BUTTONS = {"cross", "circle", "square", "triangle", "up", "down", "left", "right", "start", "select",
           "l", "r", "home", "note", "screen", "volup", "voldown"}


GUI = bool(os.environ.get("GUI"))  # GUI=1: PPSSPPSDL (a window; frames can be read back) instead of headless
if GUI:
    EBOOT = Path.home() / ".config/ppsspp/PSP/GAME/RE4/EBOOT.PBP"


def install():
    ms = EBOOT.parent
    ms.mkdir(parents=True, exist_ok=True)
    build = ROOT / "build/port"
    subprocess.run(["cp", str(build / "EBOOT.PBP"), str(EBOOT)], check=True)
    # the port's log: the emulator does not honour PSP_O_TRUNC, so a shorter run would leave the
    # previous run's tail in place
    (ms / "port.log").unlink(missing_ok=True)
    data = ms / "data"
    if not data.exists() and (build / "data").is_dir():
        data.symlink_to(build / "data")


class Emu:
    def __init__(self, ws):
        self.ws = ws
        self.pending = {}
        self.stopped = None

    async def send(self, event, **kw):
        msg = dict(event=event, **kw)
        await self.ws.send(json.dumps(msg))

    async def call(self, event, **kw):
        """Send and wait for the reply with the same event name (or an error)."""
        await self.send(event, **kw)
        while True:
            try:
                r = json.loads(await asyncio.wait_for(self.ws.recv(), 30))
            except asyncio.TimeoutError:
                print("no reply to %s within 30 s" % event)
                return {"event": "error", "message": "timeout"}
            if r.get("event") == "cpu.stepping":
                self.stopped = r  # a breakpoint hit while waiting for another reply
            if r.get("event") == event:
                return r
            if r.get("event") == "error" and r.get("level", 0) <= 3:
                return r

    async def invalidate(self, addr):
        """A breakpoint on code the JIT already compiled does not stop it (the block is not
        recompiled); writing the instruction back to itself invalidates the block."""
        r = await self.call("memory.read", address=addr & ~3, size=4)
        if "base64" in r:
            await self.call("memory.write", address=addr & ~3, base64=r["base64"])

    async def drain(self, secs):
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0:
                return
            try:
                r = json.loads(await asyncio.wait_for(self.ws.recv(), left))
            except asyncio.TimeoutError:
                return
            except Exception:
                return
            if r.get("event") == "cpu.stepping":  # a break instruction / fault stops the CPU: say where
                self.stopped = r
                pc = r.get("pc", 0)
                print("cpu stopped: %s at %08x %s" % (r.get("reason", "?"), pc, sym(pc)))


async def run(cmds, timeout, jit, log):
    install()
    if GUI:  # needs RemoteDebuggerOnStartup = True in ~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini
        args = ["PPSSPPSDL", str(EBOOT), "--windowed", "--escape-exit", "-d"]
        args.append("-j" if jit else "-i")
    else:
        args = ["PPSSPPHeadless", str(EBOOT), "--timeout=%d" % timeout, "--debugger=%d" % PORT]
        if os.environ.get("FULLLOG"):  # every emulator log line (HLE calls...): large, and it floods the debugger socket
            args.append("-l")
        args.append("-j" if jit else "-i")
        g = os.environ.get("PPSSPP_GRAPHICS", "software")  # software: the frame is in VRAM for `shot`; "none": the emulator's default (no readback)
        if g != "none":
            args.append("--graphics=" + g)
    if os.environ.get("GDB"):  # GDB=1: the emulator under gdb, backtraces of its faults in the log
        args = ["gdb", "-q", "-batch", "-ex", "handle SIGSEGV stop print", "-ex", "run"] + sum([["-ex", "bt 14", "-ex", "c"] for _ in range(4)], []) + ["--args"] + args
    logf = open(log, "w")
    proc = subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT)
    ws = None
    port = PORT
    for _ in range(150):
        if GUI:  # the GUI picks a port and logs it: "... server started on port 40077: debugger-webserver"
            m = re.search(r"started on port (\d+): debugger-webserver", open(log, errors="replace").read())
            if not m:
                await asyncio.sleep(0.2)
                continue
            port = int(m.group(1))
        try:
            ws = await websockets.connect("ws://127.0.0.1:%d/debugger" % port, max_size=64 << 20)
            break
        except OSError:
            await asyncio.sleep(0.2)
    if ws is None:
        proc.kill()
        sys.exit("could not connect to the PPSSPP debugger")
    emu = Emu(ws)
    try:
        await drive(emu, ws, proc, cmds, log)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
        logf.close()
        report(log)


async def drive(emu, ws, proc, cmds, log):
    await emu.call("cpu.resume")
    i = 0
    while i < len(cmds):
        c = cmds[i]
        i += 1
        if proc.poll() is not None:
            print("emulator exited on its own: code %s (before %r)" % (proc.returncode, c))
            break
        try:
            i = await step(emu, ws, cmds, i, c, log)
        except websockets.exceptions.ConnectionClosed:
            proc.wait(5)
            print("emulator went away during %r: exit code %s" % (c, proc.returncode))
            return
    if proc.poll() is not None:
        print("emulator exited on its own: code %s" % proc.returncode)
    try:
        await emu.send("cpu.stepping")
        await ws.close()
    except Exception:
        pass


async def step(emu, ws, cmds, i, c, log):
    if True:
        if c == "wait":
            await emu.drain(float(cmds[i])); i += 1
        elif c == "press":
            names = cmds[i].split(","); i += 1
            await emu.call("input.buttons.send", buttons={n: True for n in names})
            await emu.drain(0.2)
            await emu.call("input.buttons.send", buttons={n: False for n in names})
            await emu.drain(0.2)
        elif c == "hold":
            names = cmds[i].split(","); secs = float(cmds[i + 1]); i += 2
            r = await emu.call("input.buttons.send", buttons={n: True for n in names})
            if r.get("event") == "error":
                print("hold %s: %s" % (names, r.get("message")))
            await emu.drain(secs)
            await emu.call("input.buttons.send", buttons={n: False for n in names})
        elif c in ("down", "up"):  # down BTN[,BTN] / up BTN[,BTN]: press and hold / release
            names = cmds[i].split(","); i += 1
            await emu.call("input.buttons.send", buttons={n: c == "down" for n in names})
            await emu.drain(0.1)
        elif c == "analog":
            x, y = float(cmds[i]), float(cmds[i + 1]); i += 2
            await emu.call("input.analog.send", stick="left", x=x, y=y)
        elif c == "shot":
            path = cmds[i]; i += 1
            if os.environ.get("PPSSPP_GRAPHICS", "software") != "software":
                # a hardware backend: ask the GPU for the display buffer (a PNG data URI)
                await emu.call("cpu.stepping")  # the GPU only answers while stepping
                await emu.drain(0.3)
                r = await emu.call("gpu.buffer.screenshot", type="uri", alpha=False)
                await emu.call("cpu.resume")
                uri = r.get("uri", "")
                if "base64," in uri:
                    open(path, "wb").write(base64.b64decode(uri.split("base64,", 1)[1]))
                    print("screenshot -> %s" % path)
                else:
                    print("screenshot failed: %s" % json.dumps(r)[:300])
                return i
            # The display buffer, read out of VRAM (the software renderer keeps it there; the
            # hardware backends do not): port_gu.cpp's dispBuf holds its VRAM offset (8888, 512 wide).
            r = await emu.call("memory.read", address=symaddr("_ZL7dispBuf"), size=4)
            if "base64" not in r:
                print("screenshot failed: %s" % json.dumps(r)[:200])
                return i
            addr = 0x44000000 + int.from_bytes(base64.b64decode(r["base64"]), "little")
            stride, fmt = 512, 3
            bpp = 4
            r = await emu.call("memory.read", address=addr, size=stride * 272 * bpp)
            if "base64" not in r:
                print("screenshot failed: %s" % json.dumps(r)[:300])
                return i
            raw = base64.b64decode(r["base64"])
            from PIL import Image
            if fmt == 3:
                img = Image.frombytes("RGBA", (stride, 272), raw).crop((0, 0, 480, 272)).convert("RGB")
            else:
                mode = {0: "BGR;16", 1: "BGRA;15", 2: "BGRA;4B"}[fmt]  # 5650 / 5551 / 4444
                img = Image.frombytes("RGB", (stride, 272), raw, "raw", mode).crop((0, 0, 480, 272))
            img.save(path)
            print("screenshot -> %s" % path)
        elif c == "where":  # every thread's PC, as a function
            await emu.call("cpu.stepping")
            await emu.drain(0.3)
            r = await emu.call("hle.thread.list")
            await emu.call("cpu.resume")
            for t in r.get("threads", []):
                print("thread %-12s %-8s pc=%08x %s" % (t.get("name"), t.get("status"), t.get("pc", 0), sym(t.get("pc", 0))))
        elif c == "peek":  # peek SYMBOL[+off] SIZE: hex dump
            what, size = cmds[i], int(cmds[i + 1], 0); i += 2
            deref = what.startswith("*")  # *SYM: the address the pointer at SYM holds
            addr = symaddr(what.lstrip("*"))
            if deref:
                r = await emu.call("memory.read", address=addr, size=4)
                addr = int.from_bytes(base64.b64decode(r.get("base64", "")), "little")
                print("%s -> %08x" % (what, addr))
            r = await emu.call("memory.read", address=addr, size=size)
            raw = base64.b64decode(r.get("base64", ""))
            for o in range(0, len(raw), 16):
                print("%08x: %s" % (addr + o, " ".join("%02x" % b for b in raw[o:o + 16])))
        elif c == "poke":  # poke SYM[+off] HEX|"text": write bytes (a text is NUL-terminated)
            addr, what = symaddr(cmds[i]), cmds[i + 1]; i += 2
            data = bytes.fromhex(what) if re.fullmatch(r"([0-9a-fA-F]{2})+", what) else what.encode() + b"\0"
            r = await emu.call("memory.write", address=addr, base64=base64.b64encode(data).decode())
            print("poke %08x %d bytes: %s" % (addr, len(data), json.dumps(r)[:120]))
        elif c == "break":  # break ADDR: a breakpoint (hex address or symbol[+off])
            addr = symaddr(cmds[i]); i += 1
            r = await emu.call("cpu.breakpoint.add", address=addr, enabled=True)
            await emu.invalidate(addr)
            print("breakpoint at %08x: %s" % (addr, json.dumps(r)[:200]))
        elif c == "breakif":  # breakif ADDR COND: a conditional breakpoint (COND in the emulator's expression syntax, e.g. "a0 == 0xdeadbeef")
            addr, cond = symaddr(cmds[i]), cmds[i + 1]; i += 2
            r = await emu.call("cpu.breakpoint.add", address=addr, enabled=True, condition=cond)
            await emu.invalidate(addr)
            print("breakpoint at %08x if %s: %s" % (addr, cond, json.dumps(r)[:200]))
        elif c == "unbreak":  # unbreak ADDR: remove the breakpoint (a left-over one stops the CPU every frame)
            addr = symaddr(cmds[i]); i += 1
            await emu.call("cpu.breakpoint.remove", address=addr)
        elif c == "watch":  # watch ADDR SIZE: break on a write to the range
            addr, size = symaddr(cmds[i]), int(cmds[i + 1], 0); i += 2
            r = await emu.call("memory.breakpoint.add", address=addr, size=size, enabled=True, write=True, read=False, change=False)
            print("watchpoint at %08x+%d: %s" % (addr, size, json.dumps(r)[:200]))
        elif c == "waitbreak":  # wait (up to N s) for the CPU to stop at a breakpoint, print the PC
            secs = float(cmds[i]); i += 1
            end = time.time() + secs
            if emu.stopped:
                r, emu.stopped = emu.stopped, None
                print("stopped at %08x %s" % (r.get("pc", 0), sym(r.get("pc", 0))))
                end = 0
            while time.time() < end:
                try:
                    r = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, end - time.time())))
                except asyncio.TimeoutError:
                    break
                if r.get("event") == "cpu.stepping":
                    print("stopped at %08x %s" % (r.get("pc", 0), sym(r.get("pc", 0))))
                    break
                if os.environ.get("DEBUG"):
                    print("event:", json.dumps(r)[:200])
        elif c == "threads":  # every thread's registers (pc / ra / sp) and the top of its stack
            await emu.call("cpu.stepping")
            await emu.drain(0.3)
            r = await emu.call("hle.thread.list")
            for t in r.get("threads", []):
                tid = t.get("id")
                rr = await emu.call("cpu.getAllRegs", thread=tid)
                g = {}
                for cat in rr.get("categories", []):
                    if cat.get("name") == "GPR":
                        g = dict(zip(cat.get("registerNames", []), cat.get("uintValues", [])))
                pc = rr.get("pc", t.get("pc", 0))
                sp, ra = g.get("sp", 0), g.get("ra", 0)
                print("thread %-12s %3d st=%-2s pc=%08x %-40s ra=%08x %s sp=%08x" % (t.get("name"), tid, t.get("status", "?"), pc, sym(pc), ra, sym(ra), sp))
                if sp:
                    m = await emu.call("memory.read", address=sp, size=1024)
                    if "base64" in m:
                        words = base64.b64decode(m["base64"])
                        out = []
                        for k in range(0, len(words), 4):
                            v = int.from_bytes(words[k:k + 4], "little")
                            if 0x08800000 <= v < 0x0a000000:
                                out.append("%08x %s" % (v, sym(v)))
                        for o in out:
                            print("      stack: " + o)
        elif c == "regs":  # the CPU registers (while stopped)
            r = await emu.call("cpu.getAllRegs")
            if os.environ.get("DEBUG"):
                print("regs:", json.dumps(r)[:600])
            for cat in r.get("categories", []):
                names, vals = cat.get("registerNames", []), cat.get("uintValues", [])
                fvals = cat.get("floatValues")
                out = []
                for k, nme in enumerate(names):
                    v = vals[k] if k < len(vals) else 0
                    if cat.get("name") == "GPR":
                        REGS[nme] = v & 0xffffffff
                    out.append("%s=%08x%s" % (nme, v & 0xffffffff, ("(%s)" % fvals[k]) if fvals and k < len(fvals) and cat.get("name") == "FPU" else ""))
                print(cat.get("name"), " ".join(out))
        elif c == "waitlog":  # waitlog REGEX SECS: run until the port's log (port.log) has a matching line
            pat, secs = re.compile(cmds[i]), float(cmds[i + 1]); i += 2
            portlog = EBOOT.parent / "port.log"
            end = time.time() + secs
            found = False
            while time.time() < end and not found:
                await emu.drain(0.5)
                try:
                    found = any(pat.search(l) for l in open(portlog, errors="replace"))
                except OSError:
                    pass
            print("waitlog %r: %s" % (cmds[i - 2], "matched" if found else "timed out"))
        elif c == "stop":  # pause the CPU (regs / peek then, cont to resume)
            await emu.call("cpu.stepping")
            await emu.drain(0.3)
        elif c == "cont":
            emu.stopped = None
            await emu.call("cpu.resume")
        elif c == "quit":
            return len(cmds)
        else:
            sys.exit("unknown command %r" % c)
    return i



_syms = None


def symtab():
    global _syms
    if _syms is None:
        _syms = []
        for line in open(ROOT / "build/port/re4.nm"):
            parts = line.split()
            if len(parts) == 3:
                _syms.append((int(parts[0], 16), parts[2]))
        _syms.sort()
    return _syms


def sym(addr):
    import bisect
    t = symtab()
    i = bisect.bisect_right([a for a, _ in t], addr) - 1
    if i < 0:
        return "?"
    name = subprocess.run(["c++filt", t[i][1]], capture_output=True, text=True).stdout.strip() or t[i][1]
    return "%s+0x%x" % (name, addr - t[i][0])


REGS = {}  # the GPRs of the last `regs`


def symaddr(what):
    name, _, off = what.partition("+")
    if name in REGS:
        base = REGS[name]
    elif re.fullmatch(r"(0x)?[0-9a-fA-F]+", name):
        base = int(name, 16)
    else:
        base = [a for a, n in symtab() if n == name]
        if not base:
            print("no symbol %s (build/port/re4.nm)" % name)
            return 0
        base = base[0]
    return base + (int(off, 0) if off else 0)


def report(log):
    pat = re.compile(r"stdout|\[port\]|HALT|Exception|Illegal|Unmapped|MemoryException|breakpoint|Unimplemented|^E |^N |[Cc]rash")
    seen = {}
    lines = 0
    portlog = EBOOT.parent / "port.log"  # the port's own log on the memory stick (port_log.cpp)
    sources = [open(log, errors="replace")]
    if portlog.exists():
        sources.append(open(portlog, errors="replace"))
    for src in sources:
      for line in src:
        lines += 1
        line = line.rstrip("\n")
        if os.environ.get("RAW"):
            print(line)
            continue
        if src is not sources[0] and not line.startswith("["):
            line = "stdout: " + line  # the port.log lines have no prefix: treat them like the game's output
        if not pat.search(line) or "ISO looks bogus" in line or "lang/.ini" in line:
            continue
        line = re.sub(r"^(I )?stdout: ", "", line)[:200]
        key = re.sub(r"^\d+:[\d:.]+ +", "", line)
        if key not in seen:
            seen[key] = 1
            print(line)
    print("--- full log: %s (%d lines); the port's log: %s" % (log, lines, portlog))


def main():
    args = sys.argv[1:]
    timeout, jit, log = 100000, False, str(ROOT / "build/port/headless.log")  # the emulator counts emulated seconds, which run ahead of the clock
    while args and args[0].startswith("-"):
        a = args.pop(0)
        if a == "-t":
            timeout = int(args.pop(0))
        elif a == "-j":
            jit = True
        elif a == "--log":
            log = args.pop(0)
        else:
            sys.exit("unknown option " + a)
    asyncio.run(run(args, timeout, jit, log))


main()
