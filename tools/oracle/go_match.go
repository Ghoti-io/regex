// Go's `regexp`, behind the batch protocol every other oracle here speaks.
//
// `regexp` is RE2's lineage and the standard library's, so this is the
// reference for the `re2` dialect - and it is the *only* reference for it on
// this machine, because RE2 itself is a C++ library Debian carries without a
// matching corpus. What the two share is `regexp/syntax`, which Go's
// documentation states is RE2's syntax; what they do not share is the
// engine's choice of match, and that is a dialect cell rather than a syntax
// one (documentation/dialects.md section 5.1).
//
// **The flags are written into the pattern, because `regexp` has no other
// way to take them.** There is no `Compile(pattern, flags)` here - the flags
// are `(?ims)` groups in the pattern text, so a caller's `i` becomes a
// prefix. That is a difference from every other driver in this directory and
// it has a consequence worth stating: `(?i)` is the *dialect's own syntax*,
// so a row asking for `i` on a pattern that itself resets the flag is asking
// a question about precedence rather than about the flag.
//
// Input is one case per line, tab-separated, hex-encoded:
// `<flags>\t<pattern>\t<subject>`. Output is one line per case:
//
//	match <a:b> ...   one span per group, `-` for a group that did not take
//	nomatch
//	compile           the pattern was refused
//
// Offsets are byte offsets into the subject, which is what
// FindSubmatchIndex already returns - no conversion, unlike node's UTF-16
// and CPython's characters.
//
// Three further modes, each printing what its `grx_*` counterpart prints so
// that the existing parsers read both sides:
//
//	syntax     `ok` / `error`, reading the first two fields only
//	split      `ok <count> <hex>|<hex>|...`
//	replace    `ok <hex>` / `compile`, taking a fourth field, the template
//	all        `all <count>` then one field per match, groups comma-separated
//
// Copyright 2026 by Corey Pennycuff
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"os"
	"regexp"
	"runtime"
	"strings"
	"unicode"
)

// Every letter this dialect's alphabet has. A letter that is not here is an
// error rather than a silent zero, which is the lesson python_match.py
// records: a driver that drops an argument cannot be told from one that
// honours it, and three vectors were once written down under a flag that had
// done nothing.
//
// `x` is deliberately absent: `regexp/syntax` has no extended mode at all,
// so `x` is a letter no caller may pass here.
var flagLetters = map[byte]bool{
	'i': true, // caseless
	'm': true, // ^ and $ match at line boundaries
	's': true, // . matches a newline
	'U': true, // swap the greed of every quantifier
}

// compile applies the caller's flags and caches, the way every driver here
// does: a differential asks one pattern against many subjects.
type compiler struct {
	cache map[string]*regexp.Regexp
	bad   map[string]bool
}

func newCompiler() *compiler {
	return &compiler{cache: map[string]*regexp.Regexp{}, bad: map[string]bool{}}
}

func (c *compiler) get(flags, pattern string) (*regexp.Regexp, bool) {
	key := flags + "\x00" + pattern
	if rx, ok := c.cache[key]; ok {
		return rx, true
	}
	if c.bad[key] {
		return nil, false
	}
	text := pattern
	if flags != "" {
		text = "(?" + flags + ")" + pattern
	}
	rx, err := regexp.Compile(text)
	if err != nil {
		c.bad[key] = true
		return nil, false
	}
	c.cache[key] = rx
	return rx, true
}

func unhex(field string) (string, error) {
	raw, err := hex.DecodeString(field)
	if err != nil {
		return "", err
	}
	return string(raw), nil
}

func doMatch(c *compiler, fields []string) string {
	pattern, err := unhex(fields[1])
	if err != nil {
		return "compile"
	}
	subject, err := unhex(fields[2])
	if err != nil {
		return "compile"
	}
	rx, ok := c.get(fields[0], pattern)
	if !ok {
		return "compile"
	}
	found := rx.FindStringSubmatchIndex(subject)
	if found == nil {
		return "nomatch"
	}
	out := make([]string, 0, len(found)/2)
	for i := 0; i+1 < len(found); i += 2 {
		if found[i] < 0 {
			out = append(out, "-")
		} else {
			out = append(out, fmt.Sprintf("%d:%d", found[i], found[i+1]))
		}
	}
	return "match " + strings.Join(out, " ")
}

// doSplit is `Regexp.Split` in grx_split's output shape.
//
// The limit has the same shape CPython's `maxsplit` has and the opposite
// default: `Split(s, -1)` is every piece and `Split(s, 0)` is *no* pieces at
// all, where `re.split` reads 0 as "no limit". So a row with no limit is -1
// here and 0 is a limit meaning zero, which is a third answer again - the
// differential knows it; the driver only has to be consistent.
//
// `Split` has no group pieces: Go drops what the groups captured, where
// ECMAScript and perl interleave them. That is a dialect cell
// (GRX_SplitRule), not something to paper over here.
func doSplit(c *compiler, fields []string) string {
	pattern, err := unhex(fields[1])
	if err != nil {
		return "compile"
	}
	subject, err := unhex(fields[2])
	if err != nil {
		return "compile"
	}
	rx, ok := c.get(fields[0], pattern)
	if !ok {
		return "compile"
	}
	// The one question this transport cannot put to `Split`.
	//
	// `Regexp.Split` answers an empty subject from `len(re.expr) > 0`, and
	// this driver spells the caller's flags by *prepending* `(?ims)` to the
	// pattern - so an empty pattern with a flag arrives here four characters
	// long and takes the branch an empty one would not. The caller's question
	// was about the empty pattern and the answer would be about `(?i)`, which
	// is a wrong answer rather than a missing one, so it is refused by name.
	//
	// Only `Split` reads the expression's length, so only `Split` has this.
	// Found by split_diff.py, which reported three disagreements that were
	// all this row.
	if pattern == "" && fields[0] != "" {
		return "error flags-lengthen-expr"
	}
	limit := -1
	if len(fields) > 3 && fields[3] != "-" {
		if _, err := fmt.Sscanf(fields[3], "%d", &limit); err != nil {
			return "error"
		}
	}
	pieces := rx.Split(subject, limit)
	out := make([]string, 0, len(pieces))
	for _, piece := range pieces {
		out = append(out, hex.EncodeToString([]byte(piece)))
	}
	return fmt.Sprintf("ok %d %s", len(pieces), strings.Join(out, "|"))
}

// doReplace is `Regexp.ReplaceAllString` in grx_replace's output shape.
//
// Go's template grammar refuses nothing: `$` followed by a name that no
// group has expands to the empty string, and a trailing `$` is a literal
// one. So there is no `template` verdict to print here, which is itself the
// cell (GRX_TemplateSpec) rather than a gap in the driver.
func doReplace(c *compiler, fields []string) string {
	pattern, err := unhex(fields[1])
	if err != nil {
		return "compile"
	}
	subject, err := unhex(fields[2])
	if err != nil {
		return "compile"
	}
	template := ""
	if len(fields) > 3 {
		if template, err = unhex(fields[3]); err != nil {
			return "template"
		}
	}
	rx, ok := c.get(fields[0], pattern)
	if !ok {
		return "compile"
	}
	return "ok " + hex.EncodeToString([]byte(rx.ReplaceAllString(subject, template)))
}

// doAll is the search-all loop, which is a rule no single search can be
// asked about: which match follows an empty one is the dialect's iteration
// cell (documentation/dialects.md section 5.10).
func doAll(c *compiler, fields []string) string {
	pattern, err := unhex(fields[1])
	if err != nil {
		return "compile"
	}
	subject, err := unhex(fields[2])
	if err != nil {
		return "compile"
	}
	rx, ok := c.get(fields[0], pattern)
	if !ok {
		return "compile"
	}
	found := rx.FindAllStringSubmatchIndex(subject, -1)
	out := make([]string, 0, len(found))
	for _, one := range found {
		spans := make([]string, 0, len(one)/2)
		for i := 0; i+1 < len(one); i += 2 {
			if one[i] < 0 {
				spans = append(spans, "-")
			} else {
				spans = append(spans, fmt.Sprintf("%d:%d", one[i], one[i+1]))
			}
		}
		out = append(out, strings.Join(spans, ","))
	}
	return fmt.Sprintf("all %d %s", len(out), strings.Join(out, " "))
}

func doSyntax(c *compiler, fields []string) string {
	pattern, err := unhex(fields[1])
	if err != nil {
		return "error"
	}
	flags := ""
	if len(fields) > 0 {
		flags = fields[0]
	}
	if _, ok := c.get(flags, pattern); !ok {
		return "error"
	}
	return "ok"
}

var modes = map[string]func(*compiler, []string) string{
	"":        doMatch,
	"syntax":  doSyntax,
	"split":   doSplit,
	"replace": doReplace,
	"all":     doAll,
}

func main() {
	mode := ""
	if len(os.Args) > 1 {
		mode = os.Args[1]
	}
	if mode == "--version" {
		// The UCD version decides `\p{...}` and the case folding, and Go
		// states it in `unicode.Version` rather than anywhere on the command
		// line - so the probe reads it from the package that answers.
		fmt.Printf("%s, Unicode %s\n", runtime.Version(), unicode.Version)
		return
	}
	answer, ok := modes[mode]
	if !ok {
		fmt.Fprintln(os.Stderr, "go_match [syntax|split|replace|all]")
		os.Exit(2)
	}
	c := newCompiler()
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(make([]byte, 1<<20), 1<<24)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for in.Scan() {
		line := in.Text()
		if line == "" {
			continue
		}
		fields := strings.Split(line, "\t")
		if len(fields) < 2 {
			fmt.Fprintf(os.Stderr, "go_match: short line: %q\n", line)
			os.Exit(2)
		}
		for i := 0; i < len(fields[0]); i++ {
			if !flagLetters[fields[0][i]] {
				fmt.Fprintf(os.Stderr,
					"go_match: no such regexp flag: %q\n", fields[0][i])
				os.Exit(2)
			}
		}
		if mode != "syntax" && len(fields) < 3 {
			fmt.Fprintf(os.Stderr, "go_match: short line: %q\n", line)
			os.Exit(2)
		}
		fmt.Fprintln(out, answer(c, fields))
	}
}
