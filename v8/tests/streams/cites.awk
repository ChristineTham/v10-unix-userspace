#	A LINE CITATION IS A CLAIM ABOUT A LINE, SO IT GETS TESTED LIKE ONE.
#
# This port cites line numbers constantly -- 1132 of them, measured -- and
# CLAUDE.md already records the class: "A LINE CITATION INSIDE THE FILE IT
# CITES IS SELF-INVALIDATING", found when writing one PORT comment pushed the
# four lines it cited down by 43 and a subagent audit then found sixteen stale
# citations tree-wide.  The answer that time was five hand-written cases over
# one comment in src/sys/sys/alloc.c.  Those five are still in run.sh and are
# still the sharpest thing here, because they check what each line SAYS.
# What they are not is a population: nothing checked the other 1127.
#
# WHY A HEURISTIC WAS REJECTED.  The obvious generalisation is to pull an
# identifier out of the citing sentence and require it at the cited line.
# Measured over the whole tree that is 26% "failures", nearly all of them the
# instrument guessing -- `icheck.PORTING.md` cites `icheck.c:278` from a table
# row that also says "dcheck", and no window or adjacency rule separates a
# claim from a neighbouring word.  A test with a 26% false-positive rate is
# not a test.  So this checks the one thing that needs no guess about what a
# citation MEANS:
#
#	NOBODY CITES A BLANK LINE, A `*/', OR A BARE `break;'.
#
# A citation landing on one is stale by construction, whatever it was for.
# Measured when written: 19 findings, zero false positives, hand-verified one
# by one -- and five of the nineteen were corrections made the previous day
# that were already wrong when they were committed, which is the converge-in-
# three-measurements behaviour the class is known for.
#
# WHAT IT CANNOT SEE, said out loud because a guard's blind spot outlives it:
# a citation that drifts onto a line of plausible code.  CLAUDE.md cited
# `v8fsd.c:699` for do_walk's ENOTDIR arm; 699 had become `x[0].d_ino =
# ip->i_number;', real code, and only a human reading it caught that.  The
# structural class is the one that can be caught without guessing, not the
# whole class.
#
# RESOLUTION IS THE HARD HALF, AND THE ANSWER IS TO REFUSE.  A bare `param.h'
# names two files here -- ours and Bell Labs' -- and which one is meant is the
# `ours (upstream)' convention that src/sys/PORTING.md:1186 gets right and
# much of the tree does not.  Guessing would put the instrument back in the
# business of being wrong, so a citation resolving to anything but exactly one
# file is EXCLUDED, and the excluded count is PRINTED rather than quietly
# dropped -- the same rule the fold-call-site sweep in tests/kmemu follows.
# 846 of 1132 are excluded that way today.  That is not a gap to close by
# guessing; it is closed by writing the directory into the citation.
#
# The universe deliberately includes third_party/, because a citation to
# upstream can be wrong on the day it is written -- CLAUDE.md records
# `conf/devices:82' standing for years where it meant `:75'.
#
# usage:  awk -v ROOT=<repo root> -f cites.awk <universe> <citing-files>
#   <universe>       every path resolution may consider, one per line, relative
#   <citing-files>   every file to scan for citations, one per line, relative
# output: one `STALE <site> <target> <reason>' per finding, then one
#         `SUMMARY <checked> <excluded> <stale>'.

# --- phase 1: the resolution universe, indexed by basename ---
NR == FNR {
	nb = split($0, comp, "/")
	cand[comp[nb]] = cand[comp[nb]] " " $0
	next
}

# --- phase 2: every remaining record names a file to scan ---
{ scanfile($0) }

#	A FILE THAT CITES ITSELF SPUN THIS SCRIPT FOREVER, AND THE HANG IS THE
#	WORST PART.
#
# The first version read the citing file with `getline ln < full' and, for each
# citation, read the TARGET with `getline tl < tgt'.  awk keys an input stream
# on the FILENAME STRING, so when the target IS the citing file those two are
# one stream: cite() read it to EOF and close()d it, which rewound the outer
# loop to line 1, which re-found the same citation, which called cite() again.
# Reproduced in three lines -- one file, one self-citation -- and it does not
# fail, it SPINS at 100% with no output, which `make test' reports as nothing
# at all.
#
# THE CASE IT COULD NOT SURVIVE IS THE ONE IT WAS BUILT FOR.  CLAUDE.md's
# headline entry on this whole class is "A LINE CITATION INSIDE THE FILE IT
# CITES IS SELF-INVALIDATING" -- a citation moved by the comment doing the
# citing.  The sweep went a step without meeting one only because no file in
# the tree happened to cite itself; the first one that did wedged the suite.
#
# So nothing is read twice: every file is loaded ONCE into a cache and both
# phases read the cache.  That removes the shared-stream hazard by removing
# the second open, and it is also why the sweep is now fast -- the old one
# re-read each target file once per citation, so a citation into CLAUDE.md
# read all of CLAUDE.md again.
function loadfile(path,   full, ln, n) {
	if (path in NL) return
	full = ROOT "/" path
	n = 0
	while ((getline ln < full) > 0) L[path, ++n] = ln
	close(full)
	NL[path] = n
}

function scanfile(f,   no, rest, tok) {
	loadfile(f)
	for (no = 1; no <= NL[f]; no++) {
		rest = L[f, no]
		# the char class excludes ':' so a token holds exactly one, and
		# excludes whitespace so two citations on a line stay apart
		while (match(rest, /[A-Za-z0-9_.\/+-]+\.(c|h|s|y|l|md|sh):[0-9]+(-[0-9]+)?/)) {
			tok = substr(rest, RSTART, RLENGTH)
			rest = substr(rest, RSTART + RLENGTH)
			cite(f, no, tok)
		}
	}
}

function cite(src, lno, tok,   q, r, path, a, b, nb, comp, base, nc, list, i, p, keep, hits, tl, nl, seen, allempty, tgt) {
	split(tok, q, ":")
	path = q[1]
	split(q[2], r, "-")
	a = r[1] + 0
	b = (2 in r) ? r[2] + 0 : a
	if (b < a) { excluded++; return }		# a range read backwards is not a citation

	nb = split(path, comp, "/")
	base = comp[nb]
	nc = split(cand[base], list, " ")
	hits = 0
	for (i = 1; i <= nc; i++) {
		p = list[i]
		if (p == path || substr(p, length(p) - length(path)) == "/" path) {
			hits++
			keep = p
		}
	}
	if (hits != 1) { excluded++; return }

	loadfile(keep)			# cached; see the note above scanfile
	nl = NL[keep]; seen = 0; allempty = 1
	for (tl = a; tl <= b && tl <= nl; tl++) {
		seen++
		if (!structempty(L[keep, tl])) allempty = 0
	}

	if (a < 1 || b > nl) {
		stale++
		printf "STALE %s:%d -> %s:%s  out of range, the file has %d lines\n",
			src, lno, keep, (a == b ? a "" : a "-" b), nl
		return
	}
	if (allempty) {
		stale++
		printf "STALE %s:%d -> %s:%s  cites a structurally empty line\n",
			src, lno, keep, (a == b ? a "" : a "-" b)
		return
	}
	checked++
}

# A line no citation could be about: punctuation, comment scaffolding, or a
# bare control-flow keyword.  `return (0);' is NOT one -- it carries a value,
# and v8fs.c cites exactly that shape against fio.c.
function structempty(s,   t) {
	t = s
	sub(/^[[:space:]*\/#{}();]+/, "", t)
	sub(/[[:space:]*\/{}();]+$/, "", t)
	if (t !~ /[A-Za-z_]/) return 1
	if (t ~ /^(break|continue|return|else|do|then|fi|esac|done|endif)$/) return 1
	return 0
}

END { printf "SUMMARY %d %d %d\n", checked, excluded, stale }
