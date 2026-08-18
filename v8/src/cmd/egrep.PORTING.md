# egrep

`usr/src/cmd/egrep.y`, 629 lines, installed to `/usr/bin`. **One change**, and
it is the third instance of a class this port has already fixed twice.

## Why it is here at all

`tests/wavea` went red on 19 August 2026 on a case that had been green for
months, against a tree in which nothing relevant had changed:

```
FAIL calendar2 matches today
grep: parentheses not balanced
```

`calendar2` writes the egrep pattern for the next N days. When the window needs
more than one day **range** — 19 and 20 do not merge into one character class —
it puts the alternatives on separate **lines**, with the outer parentheses
spanning the newline:

```
(^|[ (,;])((([Aa]ug[^ ]* *|0*8/|\* */?)0*1[9-9])
(([Aa]ug[^ ]* *|0*8/|\* */?)0*2[0-0]))([^0-9,/ ]|$|/26|...)
```

That is correct for V8's own egrep and for nothing else. `egrep.y:26` is
`#define RIGHT '\n'` and `egrep.y:157` is `case RIGHT: return (OR);` — the
**lexer** turns a newline in a `-f` file into the token `|` *inside the
grammar*, so the parentheses balance and a `-f` file is one regular expression
rather than a list of them. POSIX `grep -f` reads each line as a complete
independent pattern and refuses.

The port had no egrep. `calendar3:18` runs `egrep -i -f ${T}2 '' \`cat ${T}3\``,
which inside the jail fell through the union mount to the **host's**, so
`calendar(1)` was broken on every date whose window spans two ranges — and the
suite could only see it on those dates. The host-property class with the host
property being *today*, which is the one no rerun can reproduce on demand.

Measured after: `calendar(1)` in the jail prints today's and tomorrow's entries
and not one months away.

## The build

A **bare `.y` in `cmd/`** — no directory, no makefile, no header of its own —
which is a shape no imported program has had before. Upstream's build
description is therefore `Admin/Mk`'s `*.y` arm:

```sh
*.y)	B=`basename $i .y`
	eval D=`Admin/dest $B`
	   yacc $B.y  &&
	cc $CFLAGS -o $B y.tab.c -ly &&
	install $B $D/$B &&
	rm -f y.tab.[co] $B
```

`egrep` is in **none** of `binfiles`, `etcfiles`, `libfiles`, `ulibfiles`, so
`Admin/dest` answers `/usr/bin` by fall-through — and the shipped tree agrees
(`v8/usr/bin/egrep`, 14336 bytes), so unlike `cpp` and `dump` there is no
disagreement to resolve. `$(call v8dest,egrep)` derives the same answer.

`-ly` is dropped: liby exists to supply `main()` and `yyerror()`, and `egrep.y`
defines both itself. Measured — `nm -u` on the installed binary is **empty**.

## The one change: `egrep.y:141`, `extern int yylval`

Upstream declares `yylval` `int` inside `yylex()`. This port's yacc emits
`#define YYSTYPE long` for an untyped grammar (`src/cmd/yacc/y2.c`, the
`ntypes==0` arm) — the global fix for the pointer-token bug — so the object is
eight bytes and the declaration described four. Third instance after
`awk/awk.lx.l` and `ratfor/r.h`.

**It is the one that announced itself.** Here the declaration and the
definition land in *one* translation unit, because yacc writes both into
`y.tab.c`:

```
"src/cmd/egrep.y":NNN:redeclaration of yylval from some line 76
```

(the line number in that diagnostic is the pre-fix one, spelled `NNN` here
because a stale citation and a quoted diagnostic are indistinguishable to
`cites.awk` — the hazard its own header records)

In awk and ratfor the lexer is a separate file, so nothing could speak and both
ran wrong instead. **And nothing here was silently wrong either**: the only
store is `yylval = c` at `egrep.y:188` with `c` a `char`, and every semantic value is a
character or a node index — `enter()`, `cclenter()`, `node()` and `unary()` all
return the subscript `line`. So unlike awk (seven pointer stores) and ratfor
(the token buffer's address) no pointer passes through it, and the change is
forced by the **build** rather than by an observed wrong answer.

**CLAUDE.md's documented sweep for this class could not have found it.** It
globs `src/cmd/*/*.l src/cmd/*/*.c src/cmd/*/*.h src/cmd/*/*.y` — one directory
deep — and `egrep.y` is a bare file in `cmd/`. That is the fourth narrowing of
the same sweep and the second in its **file set**, after the `.h`-and-`.g` one
ratfor exposed. Widened form:

```bash
grep -rnE '(extern|static)?[[:blank:]]*(int|short)[[:blank:]]+(yylval|yyval)\b' \
     src/cmd/*.[chly] src/cmd/*/*.[chlyg] src/cmd/*/*.[chly] |
     grep -vE '^\s*\*|PORT:'
```

## Audited and deliberately unchanged

- **No allocator is called at all** — every table is a fixed array
  (`gotofn[NSTATES][NCHARS]`, `state[]`, `chars[MAXLIN]`), so the undeclared-
  `malloc` class cannot arise.
- **No `argv[1]` walk.** The option loop at `egrep.y:427` is
  `while (--argc > 0 && (++argv)[0][0]=='-')`, which tests the count before
  the dereference — the opposite order
  from `fsck.c`'s `**++argv == '-' || --argc <= 0`, and the reason egrep is not
  an eleventh member of the address-0 table.
- **`register char c` in `yylex()` and `nextch()`** is upstream's and stays.
  It is compared against `'\0'`, `']'` and the named constants and is never used
  as a subscript, so it is not `sed -n l`'s signed-index class.
- **2 shift/reduce conflicts** are upstream's own — the grammar declares `r` in
  two separate blocks — and yacc resolves them by shifting, which is what the
  precedence declarations at `:10-13` are for.

## Not fixed, and recorded instead

`calendar4` — four lines, `gets(s)` into `char s[100]` — SIGBUSes on a path
longer than about 100 characters, which is reachable through `calendar(1)` for a
user whose `$HOME` is long. Measured by sweep: 45, 65, 85 and 105 characters
exit 0; 125 and 145 exit 138. Upstream's own on upstream's hardware, so S1 says
record it; and it is not reachable in the world this port ships, where
`tools/v8launch.sh` gives every user a home of `/usr/<name>`.
