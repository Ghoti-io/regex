" Dump vim's charclass() for every code point, for tools/check_vim_classes.py.
"
" The three pins matter more here than anywhere else in this repository,
" because charclass() *reads* two of them:
"
"   nocompatible  `-u NONE` leaves vim Vi-compatible, where 'iskeyword'
"                 defaults to `@,48-57,_` rather than the `@,48-57,_,192-255`
"                 vim's own help calls the Vim default. charclass() consults
"                 the buffer's chartab for a code point below 256, so the
"                 mode decides the answer for U+00D7 and U+00F7 - the only
"                 two members of 192-255 that vim's `@` does not cover. That
"                 is exactly the defect this gate exists to have caught:
"                 src/unicode/vim_class.c carried them as punctuation until
"                 2026-09-24 because the one-off sweep that built it ran
"                 without this line.
"   iskeyword     set explicitly as well, so the answer does not depend on
"                 the mode's default staying what it is today.
"   encoding      vim takes it from the locale; under LANG=C it reads each
"                 byte of a UTF-8 string as its own character.
set nocompatible
set encoding=utf-8
set iskeyword=@,48-57,_,192-255

let s:out = []
let s:cur = -2
let s:start = 0
let s:cp = 0
while s:cp <= 0x10FFFF
  if s:cp >= 0xD800 && s:cp <= 0xDFFF
    " nr2char() cannot make a surrogate and a UTF-8 subject cannot hold one.
    let s:c = -1
  elseif s:cp == 0
    " nr2char(0, 1) is a zero-length string - vim cannot hold NUL in one -
    " so charclass() would answer about nothing at all.
    let s:c = -1
  else
    let s:c = charclass(nr2char(s:cp, 1))
  endif
  if s:c != s:cur
    if s:cp > 0
      call add(s:out, printf('%X %X %d', s:start, s:cp - 1, s:cur))
    endif
    let s:cur = s:c
    let s:start = s:cp
  endif
  let s:cp += 1
endwhile
call add(s:out, printf('%X %X %d', s:start, 0x10FFFF, s:cur))
call writefile(s:out, g:vimclasses_out)
qa!
