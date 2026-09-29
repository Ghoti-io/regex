// The Rust `regex` crate, behind the batch protocol every other oracle here
// speaks.
//
// The reference for the `rust` dialect, and the second of the two guaranteed-
// linear dialects this library names. It is worth saying what it is *not*: it
// is not a second opinion about RE2. The two agree on the guarantee and on
// most of the grammar and disagree about the parts each added afterwards -
// `regex` has an extended mode, class set operations and `\p{...}` with the
// full set of Unicode property names, and RE2 has `\Q...\E` and `\C` - so a
// row read off one and applied to the other would be wrong in both directions.
//
// Input is one case per line, tab-separated, hex-encoded:
// `<flags>\t<pattern>\t<subject>`, plus a fourth field - the limit, or the
// template - in the two modes that take one.
//
// Output is one line per case, in the shape the matching `grx_*` driver
// prints:
//
//	(none)    `match <a:b> ...` / `nomatch` / `compile`
//	syntax    `ok` / `error`, reading the first two fields only
//	split     `ok <count> <hex>|<hex>|...`
//	replace   `ok <hex>`
//	all       `all <count>` then one field per match, groups comma-separated
//
// Offsets are byte offsets into the subject, which is what `Match::start`
// already returns: `regex` indexes `&str` by byte, so there is no conversion
// here the way node_match.mjs converts UTF-16 and python_match.py converts
// characters.
//
// Copyright 2026 by Corey Pennycuff

use std::collections::HashMap;
use std::io::{self, BufWriter, Read, Write};

use regex::{Regex, RegexBuilder};

/// Every letter this dialect's alphabet has, and an unknown one is fatal.
///
/// The lesson `tools/oracle/python_match.py` records, applied before it can
/// happen here: a driver that answers a flag it does not know by running the
/// reference with no flag at all writes down answers to a different question,
/// and nothing says so.
///
/// `u` is the crate's `unicode` switch, which is *on* by default and which a
/// caller turns off - so the letter here means "Unicode", and its absence
/// from a row's flags does not mean the ASCII semantics. That asymmetry is
/// the crate's; `rust_flags()` below is where it is spelled out.
fn build(pattern: &str, flags: &str) -> Result<Regex, ()> {
    let mut builder = RegexBuilder::new(pattern);
    // Unicode is the crate's default and `(?-u)` is how a pattern leaves it,
    // so the letter has to start where the crate starts.
    let mut unicode = true;
    for letter in flags.chars() {
        match letter {
            'i' => {
                builder.case_insensitive(true);
            }
            'm' => {
                builder.multi_line(true);
            }
            's' => {
                builder.dot_matches_new_line(true);
            }
            'U' => {
                builder.swap_greed(true);
            }
            'x' => {
                builder.ignore_whitespace(true);
            }
            'u' => {
                unicode = true;
            }
            'a' => {
                // Not a crate flag and not this dialect's letter either: it
                // is how a caller asks for `(?-u)`, the one switch here that
                // is spelled as taking something away.
                unicode = false;
            }
            other => {
                eprintln!("rust_match: no such regex flag: {:?}", other);
                std::process::exit(2);
            }
        }
    }
    builder.unicode(unicode);
    builder.build().map_err(|_| ())
}

struct Cache {
    good: HashMap<String, Regex>,
    bad: std::collections::HashSet<String>,
}

impl Cache {
    fn new() -> Cache {
        Cache { good: HashMap::new(), bad: std::collections::HashSet::new() }
    }

    fn get(&mut self, flags: &str, pattern: &str) -> Option<&Regex> {
        let key = format!("{}\u{0}{}", flags, pattern);
        if self.bad.contains(&key) {
            return None;
        }
        if !self.good.contains_key(&key) {
            match build(pattern, flags) {
                Ok(rx) => {
                    self.good.insert(key.clone(), rx);
                }
                Err(()) => {
                    self.bad.insert(key);
                    return None;
                }
            }
        }
        self.good.get(&key)
    }
}

fn unhex(field: &str) -> Option<String> {
    if field.len() % 2 != 0 {
        return None;
    }
    let mut raw = Vec::with_capacity(field.len() / 2);
    let bytes = field.as_bytes();
    let mut i = 0;
    while i + 1 < bytes.len() {
        let hi = (bytes[i] as char).to_digit(16)?;
        let lo = (bytes[i + 1] as char).to_digit(16)?;
        raw.push((hi * 16 + lo) as u8);
        i += 2;
    }
    String::from_utf8(raw).ok()
}

fn enhex(text: &str) -> String {
    text.bytes().map(|b| format!("{:02x}", b)).collect()
}

fn do_match(cache: &mut Cache, fields: &[&str]) -> String {
    let (pattern, subject) = match (unhex(fields[1]), unhex(fields[2])) {
        (Some(p), Some(s)) => (p, s),
        _ => return "compile".to_string(),
    };
    let rx = match cache.get(fields[0], &pattern) {
        Some(rx) => rx,
        None => return "compile".to_string(),
    };
    match rx.captures(&subject) {
        None => "nomatch".to_string(),
        Some(caps) => {
            let mut out = Vec::with_capacity(caps.len());
            for index in 0..caps.len() {
                match caps.get(index) {
                    None => out.push("-".to_string()),
                    Some(m) => out.push(format!("{}:{}", m.start(), m.end())),
                }
            }
            format!("match {}", out.join(" "))
        }
    }
}

fn do_syntax(cache: &mut Cache, fields: &[&str]) -> String {
    let pattern = match unhex(fields[1]) {
        Some(p) => p,
        None => return "error".to_string(),
    };
    match cache.get(fields[0], &pattern) {
        Some(_) => "ok".to_string(),
        None => "error".to_string(),
    }
}

/// `Regex::split` / `splitn`, in grx_split's output shape.
///
/// A third answer again about the limit, after CPython's 0-means-unlimited
/// and Go's 0-means-nothing: `splitn(s, n)` yields at most `n` pieces and
/// `split(s)` is every piece, so "no limit" is a different *call* rather
/// than a sentinel. Like Go's, it drops what the groups captured.
fn do_split(cache: &mut Cache, fields: &[&str]) -> String {
    let (pattern, subject) = match (unhex(fields[1]), unhex(fields[2])) {
        (Some(p), Some(s)) => (p, s),
        _ => return "compile".to_string(),
    };
    let limit = fields.get(3).copied().unwrap_or("-");
    let rx = match cache.get(fields[0], &pattern) {
        Some(rx) => rx,
        None => return "compile".to_string(),
    };
    let pieces: Vec<&str> = if limit == "-" {
        rx.split(&subject).collect()
    } else {
        match limit.parse::<usize>() {
            Ok(n) => rx.splitn(&subject, n).collect(),
            Err(_) => return "error".to_string(),
        }
    };
    let body: Vec<String> = pieces.iter().map(|p| enhex(p)).collect();
    format!("ok {} {}", pieces.len(), body.join("|"))
}

/// `Regex::replace_all`, in grx_replace's output shape.
///
/// The crate's template grammar refuses nothing either: `$` before a name no
/// group has expands to the empty string. So there is no `template` verdict
/// here, which is the dialect's cell rather than a gap in the driver.
fn do_replace(cache: &mut Cache, fields: &[&str]) -> String {
    let (pattern, subject) = match (unhex(fields[1]), unhex(fields[2])) {
        (Some(p), Some(s)) => (p, s),
        _ => return "compile".to_string(),
    };
    let template = match fields.get(3) {
        None => String::new(),
        Some(field) => match unhex(field) {
            Some(t) => t,
            None => return "template".to_string(),
        },
    };
    let rx = match cache.get(fields[0], &pattern) {
        Some(rx) => rx,
        None => return "compile".to_string(),
    };
    format!("ok {}", enhex(&rx.replace_all(&subject, template.as_str())))
}

fn do_all(cache: &mut Cache, fields: &[&str]) -> String {
    let (pattern, subject) = match (unhex(fields[1]), unhex(fields[2])) {
        (Some(p), Some(s)) => (p, s),
        _ => return "compile".to_string(),
    };
    let rx = match cache.get(fields[0], &pattern) {
        Some(rx) => rx,
        None => return "compile".to_string(),
    };
    let mut rows = Vec::new();
    for caps in rx.captures_iter(&subject) {
        let mut spans = Vec::with_capacity(caps.len());
        for index in 0..caps.len() {
            match caps.get(index) {
                None => spans.push("-".to_string()),
                Some(m) => spans.push(format!("{}:{}", m.start(), m.end())),
            }
        }
        rows.push(spans.join(","));
    }
    format!("all {} {}", rows.len(), rows.join(" "))
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mode = argv.get(1).map(|s| s.as_str()).unwrap_or("");
    if mode == "--version" {
        // The crate decides the answers, and `regex` carries its own Unicode
        // tables in `regex-syntax` rather than reading the host's - so the
        // version that matters is the crate's, which is written in here by
        // the build rather than discoverable at run time.
        println!("regex {}, rustc {}", env!("GHOTI_REGEX_CRATE"), env!("GHOTI_RUSTC"));
        return;
    }
    let answer: fn(&mut Cache, &[&str]) -> String = match mode {
        "" => do_match,
        "syntax" => do_syntax,
        "split" => do_split,
        "replace" => do_replace,
        "all" => do_all,
        _ => {
            eprintln!("rust_match [syntax|split|replace|all]");
            std::process::exit(2);
        }
    };
    let mut input = String::new();
    io::stdin().read_to_string(&mut input).expect("stdin");
    let mut cache = Cache::new();
    let stdout = io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    for line in input.lines() {
        if line.is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() < 2 || (mode != "syntax" && fields.len() < 3) {
            eprintln!("rust_match: short line: {:?}", line);
            std::process::exit(2);
        }
        writeln!(out, "{}", answer(&mut cache, &fields)).expect("stdout");
    }
}
