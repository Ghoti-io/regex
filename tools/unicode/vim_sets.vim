" Dump vim's \i, \k, \f and \p for every code point, for check_vim_sets.py.
"
" The four sets vim decides from options rather than from Unicode: `\i` is
" 'isident', `\k` is 'iskeyword', `\f` is 'isfname' and `\p` is 'isprint'.
" src/syntax/vim.c carries all four as tables and until this script existed
" nothing could reproduce any of them - they were built by a one-off sweep,
" and that file's own header records what that cost: three of the four were
" wrong when first written, and the correction to `\k` then overshot by two
" code points because the sweep ran in the wrong mode.
"
" The same three pins as tools/unicode/vim_classes.vim, and for the same
" reasons - 'iskeyword' is read directly by `\k` here, so `-u NONE` alone
" would measure Vi's default rather than vim's.
set nocompatible
set encoding=utf-8
set iskeyword=@,48-57,_,192-255

let s:sets = [['ident', '\i'], ['keyword', '\k'], ['fname', '\f'],
      \ ['print', '\p']]

let s:out = []
for s:pair in s:sets
  let s:name = s:pair[0]
  let s:pat = s:pair[1]
  let s:cur = -2
  let s:start = 0
  let s:cp = 0
  while s:cp <= 0x10FFFF
    if s:cp >= 0xD800 && s:cp <= 0xDFFF
      " nr2char() cannot make a surrogate and a UTF-8 subject cannot hold one.
      let s:in = -1
    elseif s:cp == 0
      " nr2char(0, 1) is a zero-length string, so there is nothing to ask.
      let s:in = -1
    else
      let s:in = match(nr2char(s:cp, 1), s:pat) >= 0 ? 1 : 0
    endif
    if s:in != s:cur
      if s:cp > 0
        call add(s:out, printf('%s %X %X %d', s:name, s:start, s:cp - 1, s:cur))
      endif
      let s:cur = s:in
      let s:start = s:cp
    endif
    let s:cp += 1
  endwhile
  call add(s:out, printf('%s %X %X %d', s:name, s:start, 0x10FFFF, s:cur))
endfor
call writefile(s:out, g:vimsets_out)
qa!
