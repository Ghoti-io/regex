" Dump vim's own cell width for every code point, for tools/check_vim_widths.py.
"
" `set nocompatible` because `-u NONE` leaves vim Vi-compatible, and that is
" not what a user runs - the same trap that made 'iskeyword' two code points
" narrow in this library until 2026-09-24. Nothing here is known to depend on
" it, and it is set anyway so the question never has to be asked again.
"
" `set encoding=utf-8` for the reason tools/oracle/vim_diff.py pins it: vim
" takes &encoding from the locale, and under LANG=C it reads each byte of a
" UTF-8 string as its own character.
"
" `ambiwidth` is left at its default, "single", because that is what the
" table in src/unicode/display.c was measured at. It is a user setting, so a
" vim with `set ambiwidth=double` answers differently for every East Asian
" Ambiguous code point - which is a dialect this library does not offer and
" must not silently acquire from whoever runs the gate.
set nocompatible
set encoding=utf-8
set ambiwidth=single

let s:out = []
let s:cur = -1
let s:start = 0
let s:cp = 0
while s:cp <= 0x10FFFF
  if s:cp >= 0xD800 && s:cp <= 0xDFFF
    " nr2char() cannot make a surrogate and a UTF-8 subject cannot hold one,
    " so it is written through rather than probed - the same convention
    " src/syntax/vim.c's option-backed tables use.
    let s:w = -1
  else
    " The *contribution* after a base character, not the width of a lone
    " one. strdisplaywidth() of an isolated combining character is vim's
    " escape rendering - `<180b>` is six columns - which is a question about
    " how vim draws an unprintable, not about how many cells the character
    " takes. Nine code points differ that way and none of them is a
    " disagreement: U+180B..U+180D, U+302A..U+302D, U+3099 and U+309A.
    "
    " The delta answers the question grx_display_cell_width(cp, 0) asks, and
    " for everything that is not a combining character it is identical to
    " the lone width - "a" plus a wide character is three columns.
    let s:w = strdisplaywidth("a" . nr2char(s:cp, 1)) - 1
  endif
  if s:w != s:cur
    if s:cp > 0
      call add(s:out, printf('%X %X %d', s:start, s:cp - 1, s:cur))
    endif
    let s:cur = s:w
    let s:start = s:cp
  endif
  let s:cp += 1
endwhile
call add(s:out, printf('%X %X %d', s:start, 0x10FFFF, s:cur))
call writefile(s:out, g:vimwidths_out)
qa!
