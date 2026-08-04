---
name: port-program
description: Port a V8 program from third_party into the build — import with provenance, audit for LP64 and Mach-O hazards, wire it up with correct dependencies, and record the port. Use when adding a new Bell Labs program to this port.
disable-model-invocation: true
---

# Porting a V8 program

Seven steps. **Steps 1, 4 and 6 are the ones that get skipped**, and each has a
history of costing multi-round debugging here.

Argument: the program name, e.g. `spell`. If none was given, ask which.

## 1. Import — never copy by hand

```bash
tools/import.sh v8/usr/src/cmd/NAME
```

This mirrors the path under `src/` and records each file's upstream git blob
hash in `PROVENANCE`, which is what keeps the diff against pristine V8
reconstructible. A hand copy loses that silently, and a `PreToolUse` hook will
refuse a direct edit to `third_party/` anyway.

If the program is a single file it is `v8/usr/src/cmd/NAME.c`.

## 2. Audit before building, not after

```bash
.claude/skills/port-program/audit.sh src/cmd/NAME
```

Then run the **lp64-auditor** subagent over the directory for the judgement
calls the greps cannot make — especially arrays whose type is declared in one
file and whose allocation lives in another. That exact split (lex's `once.c`
declaring `long *left`, `parser.y` allocating it with `sizeof(*left)`) produced
a 2× heap overrun that presented as "calloc returns 0" and took a full session.

Do not "fix" what is merely old. K&R declarations, implicit `int`, `register`
and bare `return;` are correct here. Only changes forced by LP64, Mach-O or the
ARM64 ABI are legitimate, and each one goes in `PORTING.md` with its reason.

## 3. Makefile block

Model it on an existing program of similar shape — `spell` for several small
programs sharing objects, `tbl` for one program with many objects, `make` for
one needing yacc.

- Take the object list and any dependency lines from the program's **own V8
  makefile** if it has one (`src/cmd/NAME/[Mm]akefile`). They carry real
  knowledge: `src/cmd/lex/Makefile:11` already declares the dependency whose
  absence caused the lex bug.
- Object rules take `$(V8CC_DEPS)`. Link rules take `$(V8DEPS)`, `$(V8LIBS)`,
  `$(V8LDFLAGS)`. Never respell the library list.
- **Never introduce a variable below its first use.** Make expands variables in
  target names and prerequisites when it *reads* the rule, so one defined lower
  down expands to nothing and the dependency silently does not exist. This has
  happened three times. A `PostToolUse` hook now catches it.
- One target per rule. `a b: dep` is two rules sharing a recipe under make 3.81
  and races under `-j`.

## 4. Declare every `#include`d non-header

From step 2's output. These are invisible to dependency scanning *and* to a
`*.c` glob, so they go stale silently — which is what a stale `y.tab.o` did to
lex.

```make
$(NAME_OBJ): $(NAMESRC)/thatfile.c
```

## 5. Wire it in

Add the program to `.PHONY` and to the `stage0` target. If it belongs in the V8
`/bin`, add it to `V8BIN` so it is installed into the rootfs and reachable
inside the jail.

## 6. Add dependency cases

Add cases to `tests/deps/run.sh` for the new rules — including step 4's, and the
rootfs install copy if there is one. Then **verify by mutation**: delete the
dependency line, confirm the test fails, restore it. A guard that has never been
seen to fail is not a guard.

## 7. Record and test

Write `src/cmd/NAME/PORTING.md`:

- what changed and **why** — the machine-level reason, not the symptom
- what was eliminated by measurement, so nobody re-investigates it
- what is still open, with the exact next question

Then add cases to the relevant wave suite (`tests/wavea|waveb|wavec/run.sh`)
that run the real program on real input, not synthetic exercises. Every
back-end bug in this port has lived in combinations of features that real code
uses and unit tests do not.

Finally:

```bash
make -j8 && make test
```

## Done when

The program builds from clean, `tests/deps` covers its rules, its suite passes,
and `PORTING.md` would let someone else pick up whatever is still open.
