#!/usr/bin/env python3
"""Drive the port in PPSSPPHeadless through its WebSocket debugger: boot the EBOOT, wait, press
buttons, take screenshots, read the game's log. Needs the `websockets` package (pip).

    ppsspp.py [-t SECS] [-j] [--log FILE] CMD...      GUI=1 in the environment: PPSSPPSDL instead of headless
      wait N            run for N seconds
      press BTN[,BTN]   press buttons (cross circle square triangle up down left right start
                        select l r) for 0.2 s;  hold BTN N;  down BTN / up BTN
      shot FILE.png     screenshot of the current frame
      analog X Y        left stick (-1..1)
      where             every thread's PC as a function (build/port/re4.nm)
      peek SYM[+off] N  hex dump N bytes at a symbol (or hex address)
      quit              stop the emulator (implied at the end)
    -t: PPSSPPHeadless --timeout (default 600)   -j: JIT instead of the interpreter
The emulator's log goes to --log (default build/port/headless.log); the game's stdout lines and
faults are printed at the end, de-duplicated (RAW=1 in the environment prints everything).
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
    data = ms / "data"
    if not data.exists() and (build / "data").is_dir():
        data.symlink_to(build / "data")


class Emu:
    def __init__(self, ws):
        self.ws = ws
        self.pending = {}

    async def send(self, event, **kw):
        msg = dict(event=event, **kw)
        await self.ws.send(json.dumps(msg))

    async def call(self, event, **kw):
        """Send and wait for the reply with the same event name (or an error)."""
        await self.send(event, **kw)
        while True:
            r = json.loads(await asyncio.wait_for(self.ws.recv(), 30))
            if r.get("event") == event:
                return r
            if r.get("event") == "error" and r.get("level", 0) <= 3:
                return r

    async def drain(self, secs):
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0:
                return
            try:
                await asyncio.wait_for(self.ws.recv(), left)
            except asyncio.TimeoutError:
                return


async def run(cmds, timeout, jit, log):
    install()
    if GUI:  # needs RemoteDebuggerOnStartup = True in ~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini
        args = ["PPSSPPSDL", str(EBOOT), "--windowed", "--escape-exit", "-d"]
        args.append("-j" if jit else "-i")
    else:
        args = ["PPSSPPHeadless", str(EBOOT), "--timeout=%d" % timeout, "-l", "--debugger=%d" % PORT]
        args.append("-j" if jit else "-i")
        args.append("--graphics=" + os.environ.get("PPSSPP_GRAPHICS", "software"))  # software: the frame is in VRAM for `shot`
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
    await emu.call("cpu.resume")
    i = 0
    while i < len(cmds):
        c = cmds[i]
        i += 1
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
            await emu.call("input.buttons.send", buttons={n: True for n in names})
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
            # The display buffer, read out of VRAM (the software renderer keeps it there; the
            # hardware backends do not): the last sceDisplaySetFrameBuf in the HLE log names it.
            fb = re.findall(r"sceDisplaySetFrameBuf\(([0-9a-f]+), (\d+), (\d+), \d+\)", open(log, errors="replace").read())
            if not fb:
                print("screenshot: no sceDisplaySetFrameBuf in the log yet")
                continue
            addr, stride, fmt = int(fb[-1][0], 16), int(fb[-1][1]), int(fb[-1][2])
            bpp = 4 if fmt == 3 else 2
            r = await emu.call("memory.read", address=addr, size=stride * 272 * bpp)
            if "base64" not in r:
                print("screenshot failed: %s" % json.dumps(r)[:300])
                continue
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
        elif c == "break":  # break ADDR: a breakpoint (hex address or symbol[+off])
            addr = symaddr(cmds[i]); i += 1
            await emu.call("cpu.breakpoint.add", address=addr, enabled=True)
        elif c == "waitbreak":  # wait (up to N s) for the CPU to stop at a breakpoint, print the PC
            secs = float(cmds[i]); i += 1
            end = time.time() + secs
            while time.time() < end:
                try:
                    r = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, end - time.time())))
                except asyncio.TimeoutError:
                    break
                if r.get("event") == "cpu.stepping":
                    print("stopped at %08x %s" % (r.get("pc", 0), sym(r.get("pc", 0))))
                    break
        elif c == "regs":  # the CPU registers (while stopped)
            r = await emu.call("cpu.getAllRegs")
            for cat in r.get("categories", []):
                names, vals = cat.get("names", []), cat.get("uintValues", cat.get("floatValues", []))
                fvals = cat.get("floatValues")
                out = []
                for k, nme in enumerate(names):
                    v = vals[k] if k < len(vals) else 0
                    out.append("%s=%08x%s" % (nme, v & 0xffffffff, ("(%g)" % fvals[k]) if fvals and cat.get("name") == "FPU" else ""))
                print(cat.get("name"), " ".join(out))
        elif c == "cont":
            await emu.call("cpu.resume")
        elif c == "quit":
            break
        else:
            sys.exit("unknown command %r" % c)
    try:
        await emu.send("cpu.stepping")
    except Exception:
        pass
    await ws.close()
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    logf.close()
    report(log)


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


def symaddr(what):
    name, _, off = what.partition("+")
    if re.fullmatch(r"(0x)?[0-9a-fA-F]+", name):
        base = int(name, 16)
    else:
        base = [a for a, n in symtab() if n == name]
        if not base:
            sys.exit("no symbol %s" % name)
        base = base[0]
    return base + (int(off, 0) if off else 0)


def report(log):
    pat = re.compile(r"stdout|\[port\]|HALT|Exception|Illegal|Unmapped|MemoryException|breakpoint|Unimplemented|^E |^N |[Cc]rash")
    seen = {}
    lines = 0
    for line in open(log, errors="replace"):
        lines += 1
        line = line.rstrip("\n")
        if os.environ.get("RAW"):
            print(line)
            continue
        if not pat.search(line) or "ISO looks bogus" in line or "lang/.ini" in line:
            continue
        line = re.sub(r"^I stdout: ", "", line)[:200]
        key = re.sub(r"^\d+:[\d:.]+ +", "", line)
        if key not in seen:
            seen[key] = 1
            print(line)
    print("--- full log: %s (%d lines)" % (log, lines))


def main():
    args = sys.argv[1:]
    timeout, jit, log = 600, False, str(ROOT / "build/port/headless.log")
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
