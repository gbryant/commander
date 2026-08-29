"""gdb helpers that know commander's structures.

Loaded automatically by `./debug` (scripts/swd.sh passes -x when this file is
present in the fetched framework), or by hand:

    (gdb) source /path/to/commander/scripts/gdb/commander.py

The point is the two failures that look exactly like broken hardware from
outside the firmware, and are one command away from inside it:

  * a module registered but **never pumped** — its tick() isn't in the ticker
    list, so it does nothing at all, which is indistinguishable from a wiring
    fault until you look;
  * a command silently **dropped** because MAX_COMMANDS was too small — the
    module is there, the command just isn't.

    (gdb) cmdr-tickers
    (gdb) cmdr-commands
    (gdb) cmdr-modules
    (gdb) cmdr-panic

Everything here is read-only and needs no code change: these are private
members, which gdb reads happily given DWARF. Nothing calls into the target
either — module identity comes from the *static* type of each vtable pointer
(gdb resolves it from the vtable without executing anything), so these work on a
target that is halted, faulted or otherwise in no state to run an inferior call.
"""
import re

import gdb


# ── finding the objects ──────────────────────────────────────────────────────
#
# In a cmdr project the registry and transport are `static` objects in the
# generated commander_modules.h or in the runner: internal linkage, so there is
# no predictable global name to look up. They are in DWARF, though, so find them
# by their type in `info variables` output rather than by guessing a name.

_DECL = re.compile(r"^\s*(?:\d+:\s*)?(?:static\s+)?(?:const\s+)?"
                   r"(?P<type>[\w:]+)\s+(?P<name>[\w:]+)\s*(?:\[\d*\])?\s*;")


def _candidates(type_name):
    """Names of variables declared with the given type, in declaration order."""
    try:
        out = gdb.execute("info variables", to_string=True)
    except gdb.error:
        return []
    found = []
    for line in out.splitlines():
        m = _DECL.match(line)
        if m and m.group("type").split("::")[-1] == type_name:
            name = m.group("name")
            if name not in found:
                found.append(name)
    return found


def _resolve(type_name, arg):
    """The object to inspect: an explicit expression if given, else the single
    variable of this type. Ambiguity is reported rather than guessed at."""
    if arg:
        return gdb.parse_and_eval(arg)
    names = _candidates(type_name)
    if not names:
        raise gdb.GdbError(
            f"no {type_name} found in the symbol table.\n"
            f"If this firmware has one under a name gdb can't see, pass it:\n"
            f"    (gdb) {type_name} is at <expr>\n"
            f"e.g. cmdr-commands g_registry\n"
            f"(If nothing resolves at all, the image was probably built without\n"
            f"debug info, or you are attached to a different image than the ELF.)")
    if len(names) > 1:
        raise gdb.GdbError(
            f"several {type_name} objects: {', '.join(names)}\n"
            f"Name the one you mean, e.g. `cmdr-commands {names[0]}`.")
    for n in names:
        try:
            return gdb.parse_and_eval(n)
        except gdb.error:
            continue
    raise gdb.GdbError(f"found {names} but could not read them")


def _cstr(val):
    """A const char* as text, tolerating a null or unreadable pointer."""
    try:
        if int(val) == 0:
            return "(null)"
        return val.string()
    except (gdb.MemoryError, gdb.error, UnicodeDecodeError):
        return "(unreadable)"


def _class_of(ptr):
    """The concrete class behind a module pointer, without calling anything on
    the target.

    Two routes, in order. A typed pointer (the ticker list is `IModule *`)
    carries a vtable, so gdb resolves `dynamic_type` directly. A command's `ctx`
    is a bare `void *` with no vtable to resolve, so instead name the symbol at
    that address and read *its* declared type — accurate for anything with a
    symbol, and unlike casting to IModule* it stays quiet when the object isn't
    a module at all (the `help` command's ctx is the registry itself)."""
    try:
        target = ptr.dynamic_type.target()
        name = str(target.name or target)
        if name not in ("void", "IModule"):
            return name
    except (gdb.error, AttributeError):
        pass

    sym = _symbol_at(ptr)
    if sym == "?":
        return "?"
    try:
        t = gdb.parse_and_eval(sym).type.strip_typedefs()
        return str(t.name or t)
    except (gdb.error, AttributeError, ValueError):
        return sym


def _symbol_at(ptr):
    """The symbol name at a pointer, or "?" — `info symbol` without the section."""
    try:
        out = gdb.execute(f"info symbol {int(ptr)}", to_string=True).strip()
    except (gdb.error, ValueError):
        return "?"
    if not out or out.startswith("No symbol"):
        return "?"
    return out.split(" in section")[0].strip() or "?"


# ── the commands ─────────────────────────────────────────────────────────────

class CmdrCommands(gdb.Command):
    """cmdr-commands [expr] — the shell's command table.

Prints every registered command with its help text and I2C id, and loudly flags
any that were dropped because MAX_COMMANDS was too small. A dropped command is
silent at runtime: the module registered fine, the command just isn't there."""

    def __init__(self):
        super().__init__("cmdr-commands", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        reg = _resolve("CommandRegistry", arg.strip())
        count = int(reg["_count"])
        dropped = int(reg["_dropped"])
        # Capacity from the array itself. Taking fields()[0] instead was wrong:
        # the first data member of the class is not necessarily _commands.
        cmds = reg["_commands"]
        cap = int(cmds.type.range()[1]) + 1

        print(f"{count} command(s) registered, capacity {cap}:\n")
        print(f"  {'name':<14} {'id':>4}  help")
        print(f"  {'-' * 14} {'-' * 4}  {'-' * 44}")
        for i in range(count):
            c = cmds[i]
            print(f"  {_cstr(c['name']):<14} {int(c['i2c_id']):>4}  {_cstr(c['help'])[:44]}")

        if dropped:
            first = _cstr(reg["_firstDropped"])
            print(f"\n  *** {dropped} COMMAND(S) DROPPED — MAX_COMMANDS is {cap}, too small. ***")
            print(f"  *** First dropped: '{first}'. Those commands do not exist at")
            print(f"  *** runtime; the modules registered fine. Raise MAX_COMMANDS")
            print(f"  *** (cmdr sizes it from the enabled set — `cmdr regen`).")
        else:
            print(f"\n  none dropped ({cap - count} slot(s) spare)")


class CmdrTickers(gdb.Command):
    """cmdr-tickers [expr] — the modules being pumped from the transport loop.

A module whose tick() never runs presents as dead hardware. This is the list
that decides, so compare it against cmdr-modules: anything registered but not
here does nothing periodic, however correct its driver is."""

    def __init__(self):
        super().__init__("cmdr-tickers", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        tr = _resolve("UartTransport", arg.strip())
        count = int(tr["_tickCount"])
        dropped = int(tr["_tickDropped"])
        tickers = tr["_tickers"]
        cap = int(tickers.type.range()[1]) + 1

        print(f"{count} ticker(s) of {cap}:\n")
        for i in range(count):
            p = tickers[i]
            print(f"  [{i}] {_class_of(p):<24} {p}")
        if count == 0:
            print("  (none — nothing is being pumped)")

        if dropped:
            print(f"\n  *** {dropped} TICKER(S) DROPPED — COMMANDER_MAX_TICKERS is {cap}. ***")
            print(f"  *** Those modules' tick() never runs: they will look like dead")
            print(f"  *** hardware while their commands still answer.")
        else:
            print(f"\n  none dropped ({cap - count} slot(s) spare)")


class CmdrModules(gdb.Command):
    """cmdr-modules [expr] — registered modules, by concrete class.

Read from the command table's handler contexts and the ticker list, so it needs
no separate module registry and calls nothing on the target."""

    def __init__(self):
        super().__init__("cmdr-modules", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        seen = {}
        try:
            tr = _resolve("UartTransport", "")
            for i in range(int(tr["_tickCount"])):
                p = tr["_tickers"][i]
                seen.setdefault(str(p), [_class_of(p), True, []])
        except gdb.GdbError:
            pass                       # no transport (libraries-only, or not yet up)

        reg = _resolve("CommandRegistry", arg.strip())
        for i in range(int(reg["_count"])):
            c = reg["_commands"][i]
            ctx = c["ctx"]
            if int(ctx) == 0:
                continue               # a free function, not a module method
            e = seen.setdefault(str(ctx), [None, False, []])
            if e[0] is None:
                e[0] = _class_of(ctx)
            e[2].append(_cstr(c["name"]))

        if not seen:
            print("no modules found")
            return
        print(f"{'class':<24} {'ticks':<6} commands")
        print(f"{'-' * 24} {'-' * 6} {'-' * 30}")
        for addr, (cls, ticks, cmds) in seen.items():
            mark = "yes" if ticks else "no"
            print(f"{cls:<24} {mark:<6} {', '.join(cmds) or '—'}")
        print("\nA module with commands but ticks=no does nothing periodic — if it")
        print("was meant to, its tick() is not in the ticker list (see cmdr-tickers).")


class CmdrPanic(gdb.Command):
    """cmdr-panic — whether the target is sitting in commander_on_panic().

The default weak implementation is an infinite loop, so a panicked board looks
like a hang. This says so, and names the caller — validateIds() gets here when
two commands claim the same I2C id."""

    def __init__(self):
        super().__init__("cmdr-panic", gdb.COMMAND_STATUS)

    def invoke(self, arg, from_tty):
        try:
            frame = gdb.newest_frame()
        except gdb.error:
            raise gdb.GdbError("no stack — is the target halted? (monitor halt)")

        chain, hit = [], None
        while frame is not None:
            fname = frame.name() or "??"
            chain.append(fname)
            if hit is None and "commander_on_panic" in fname:
                hit = len(chain) - 1
            frame = frame.older()

        if hit is None:
            print("not in commander_on_panic — no commander panic on this stack.")
            print("(That does not rule out a fault: check `monitor reg` / the")
            print(" HardFault handler if the target is stuck elsewhere.)")
            return

        print("*** commander_on_panic() — the board is spinning here, not hung. ***\n")
        for i, f in enumerate(chain):
            print(f"  #{i} {f}" + ("   <-- panic" if i == hit else ""))
        caller = chain[hit + 1] if hit + 1 < len(chain) else "(unknown)"
        print(f"\nCalled from {caller}.")
        if "validateIds" in caller:
            print("validateIds() panics when two commands claim the same I2C id —")
            print("run cmdr-commands and look for a duplicate in the id column.")


CmdrCommands()
CmdrTickers()
CmdrModules()
CmdrPanic()
print("commander gdb helpers: cmdr-commands, cmdr-tickers, cmdr-modules, cmdr-panic")
