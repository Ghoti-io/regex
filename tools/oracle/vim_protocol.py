"""Fail-closed Vim string oracle transport, shared by both differentials.

Exception categories come from the pinned Vim user manual (pattern.txt and
message.txt) and measured black-box errors. No implementation source is used.
E33 is a contextual refusal: these fresh, string-only runs have no previous
substitute state. It is never permission to reuse a previous pattern.
"""
import json
import re
import subprocess
from pathlib import Path

# Positively identified malformed-pattern / unavailable string-context errors.
# Resource exhaustion (including E363), internal and unknown errors stay errors.
REFUSAL_CATEGORIES = {
    33: 'previous substitute state unavailable in fresh string contract',
    51: 'more than nine capturing groups',
    53: 'unmatched noncapturing opening group',
    54: 'unmatched opening group',
    55: 'unmatched closing group',
    61: 'nested star',
    62: 'nested assertion operator',
    63: 'invalid newline-inclusive atom escape',
    64: 'postfix operator without an atom',
    65: 'illegal back reference',
    66: 'syntax-file opening group unavailable in string contract',
    67: 'syntax-file back reference unavailable in string contract',
    68: 'invalid character after backslash-z',
    70: 'empty optional sequence',
    369: 'invalid optional sequence item',
    554: 'malformed counted repeat',
    866: 'misplaced NFA operator',
    867: 'unknown NFA pattern operator',
    871: 'NFA postfix operator following another postfix operator',
    872: 'more than nine NFA capturing groups',
    888: 'forbidden marker repetition',
}
REFUSALS = frozenset(REFUSAL_CATEGORIES)


def loads(value):
    """A repeated JSON key is ambiguous evidence, even if its last value is valid."""
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate reference JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(value, object_pairs_hook=unique)


def classify_exception(exception, phase='pattern'):
    # A replacement expression can invoke another regex (or eval) and raise
    # these same codes at runtime. Only the direct outer-pattern phase proves
    # that a syntax-shaped exception actually refuses the pattern under test.
    if phase != 'pattern' or not isinstance(exception, str):
        return 'error'
    found = re.fullmatch(r'Vim(?:\([^\n()]*\))?:E([0-9]+): ([^\n]+)', exception)
    if found and int(found[1]) in REFUSALS:
        return 'refused'
    # E877 belongs to the manual's broader NFA debugging/unimplemented group.
    # Qualify only the measured malformed \_ character-class refusal; another
    # E877 message must not silently become a pattern refusal.
    if (found and int(found[1]) == 877 and re.fullmatch(
            r'\(NFA regexp\) Invalid character class: [0-9]+', found[2])):
        return 'refused'
    return 'error'


# The catch is deliberately unclassified. Python validates the envelope and
# classifies its exact exception before any comparison can use its answer.
def catch(replacement=False):
    isolate = """    try
      call matchstrpos('', c[0])
    catch
      let answer.pattern_exception = v:exception
      let answer.pattern_throwpoint = v:throwpoint
    endtry
""" if replacement else ''
    return """  catch
    let answer = {'status':'exception', 'exception':v:exception, 'throwpoint':v:throwpoint, 'phase':phase}
""" + isolate + """  endtry
  let answer.id = len(out)
  let answer.input = c
  call add(out, json_encode(answer))
"""


CATCH = catch()


def classify_answer(answer):
    original = answer.get('exception')
    phase = answer.get('phase')
    if phase == 'pattern':
        return classify_exception(original)
    if phase == 'replacement':
        isolated = answer.get('pattern_exception')
        # The same numeric category must be independently reproduced by a
        # direct outer-pattern call. A nested regex/eval syntax error alone
        # says nothing about the outer pattern or replacement grammar.
        if (classify_exception(original) == 'refused'
                and classify_exception(isolated) == 'refused'
                and re.search(r':E[0-9]+:', original).group() ==
                    re.search(r':E[0-9]+:', isolated).group()):
            return 'refused'
    return 'error'


class Failure(RuntimeError):
    def __init__(self, message, evidence):
        super().__init__(message)
        self.evidence = evidence


class Answers(list):
    def __init__(self, answers, evidence):
        super().__init__(answers)
        self.evidence = evidence


def text(value):
    return value.decode('utf-8', 'replace') if isinstance(value, bytes) else (value or '')


def lines(value):
    """File transport is LF delimited; JSON strings may contain U+0085/U+2028."""
    rows = value.split('\n')
    if rows and rows[-1] == '':
        rows.pop()
    return rows


def diagnostics(stderr):
    """Only the exact container startup notice is non-diagnostic."""
    return [line for line in text(stderr).splitlines() if line !=
        'Emulate Docker CLI using podman. Create /etc/containers/nodocker to quiet msg.']


def channels(stdout, stderr):
    """Readable channels plus lossless bytes, including invalid UTF-8."""
    def raw(value):
        return value if isinstance(value, bytes) else text(value).encode('utf-8')
    return dict(stdout=text(stdout), stderr=text(stderr),
                stdout_hex=raw(stdout).hex(), stderr_hex=raw(stderr).hex())


def run_driver(command, payload, timeout=900):
    """Library status and diagnostics are independent of its answer count."""
    try:
        finished = subprocess.run(command, input=payload.encode('utf-8'),
                                  capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise Failure('library driver failed', dict(command=command,
            **channels(getattr(error, 'stdout', ''), getattr(error, 'stderr', '')),
            error=str(error))) from error
    evidence = dict(command=command, returncode=finished.returncode,
                    **channels(finished.stdout, finished.stderr))
    if finished.returncode or finished.stderr:
        raise Failure('library driver failed', evidence)
    try:
        stdout = (finished.stdout.decode('utf-8') if isinstance(finished.stdout, bytes)
                  else finished.stdout)
    except UnicodeError as error:
        raise Failure('invalid UTF-8 from library driver', evidence) from error
    return subprocess.CompletedProcess(command, finished.returncode, stdout, text(finished.stderr))


def validate_envelope(answer):
    if (not isinstance(answer, dict) or type(answer.get('id')) is not int
            or answer['id'] < 0 or not isinstance(answer.get('input'), list)
            or len(answer['input']) not in (2, 3)
            or any(not isinstance(value, str) for value in answer['input'])):
        raise ValueError('malformed reference answer identity')
    status = answer.get('status')
    keys = dict(exception={'status', 'id', 'input', 'exception', 'throwpoint', 'phase'},
                match={'status', 'id', 'input', 'fields'},
                nomatch={'status', 'id', 'input'},
                ok={'status', 'id', 'input', 'hex'})
    expected_keys = keys.get(status) if isinstance(status, str) else None
    if status == 'exception' and ('pattern_exception' in answer or 'pattern_throwpoint' in answer):
        expected_keys = expected_keys | {'pattern_exception', 'pattern_throwpoint'}
    if expected_keys is None or set(answer) != expected_keys:
        raise ValueError('malformed reference status fields')
    if status == 'exception' and (not isinstance(answer['exception'], str)
                                 or not isinstance(answer['throwpoint'], str)
                                 or answer['phase'] not in ('pattern', 'replacement')):
        raise ValueError('malformed reference exception')
    if status == 'exception':
        if ((len(answer['input']) == 2 and answer['phase'] != 'pattern')
                or (len(answer['input']) == 3 and answer['phase'] != 'replacement')):
            raise ValueError('reference exception phase does not match operation')
        if 'pattern_exception' in answer and (answer['phase'] != 'replacement'
                or not isinstance(answer['pattern_exception'], str)
                or not isinstance(answer['pattern_throwpoint'], str)):
            raise ValueError('malformed outer-pattern isolation evidence')


def run(command, out_path, inputs, timeout=1800):
    """Retain every available diagnostic, even complete output on process failure."""
    evidence = dict(command=command, inputs=inputs, complete=False,
                    stdout='', stderr='', stdout_hex='', stderr_hex='', raw_answers='', returncode=None)
    try:
        finished = subprocess.run(command, stdin=subprocess.DEVNULL,
                                  capture_output=True, timeout=timeout)
        evidence.update(returncode=finished.returncode,
                        **channels(finished.stdout, finished.stderr))
    except (OSError, subprocess.TimeoutExpired) as error:
        evidence.update(**channels(getattr(error, 'stdout', ''), getattr(error, 'stderr', '')),
                        error=str(error))
    path = Path(out_path)
    if path.exists():
        raw = path.read_bytes()
        evidence['raw_answers'] = raw.decode('utf-8', 'replace')
        evidence['raw_answers_hex'] = raw.hex()
    else:
        raw = b''
    return validate_process(evidence, inputs, path.exists())


def validate_process(evidence, inputs, output_exists=True):
    """Validate saved process evidence without changing its local identities."""
    raw = bytes.fromhex(evidence.get('raw_answers_hex', ''))
    # The container CLI's one known startup notice is not a Vim diagnostic.
    # Match its complete line, preserving it verbatim in evidence; a diagnostic
    # containing these words still invalidates the run.
    if (evidence.get('error') or evidence['returncode'] != 0
            or evidence['stdout'] or diagnostics(evidence['stderr']) or not output_exists):
        raise Failure('Vim reference process failed', evidence)
    try:
        rows = lines(raw.decode('utf-8'))
        if len(rows) != len(inputs):
            raise ValueError('incomplete reference population')
        replies = []
        for index, (line, expected) in enumerate(zip(rows, inputs)):
            answer = loads(line)
            validate_envelope(answer)
            if answer['id'] != index or answer['input'] != expected:
                raise ValueError('malformed or reordered reference answer')
            status = answer['status']
            if status == 'exception':
                answer['classification'] = classify_answer(answer)
                if answer['classification'] == 'error':
                    raise ValueError('reference execution error: ' + answer['exception'])
            replies.append(answer)
    except (ValueError, UnicodeError) as error:
        evidence['error'] = str(error)
        raise Failure(str(error), evidence) from error
    evidence['complete'] = True
    return Answers(replies, evidence)


def match_answer(answer, subject):
    status = answer.get('status')
    if status == 'exception' and classify_answer(answer) == 'refused':
        return 'compile'
    if status == 'nomatch':
        return 'nomatch'
    if status != 'match':
        raise ValueError('invalid reference match status: %r' % answer)
    fields = answer.get('fields')
    if not isinstance(fields, list) or len(fields) != 10:
        raise ValueError('reference must supply exactly nine group texts')
    validate_match_fields(fields, subject)
    return 'match ' + json.dumps(fields)


def validate_match_fields(fields, subject):
    if (not isinstance(fields, list) or not 1 <= len(fields) <= 10
            or any(not isinstance(field, str) for field in fields)
            or re.fullmatch(r'[0-9]+:[0-9]+', fields[0]) is None):
        raise ValueError('invalid reference match fields: %r' % fields)
    for field in fields:
        field.encode('utf-8')
    start, end = map(int, fields[0].split(':'))
    raw = subject.encode('utf-8')
    if not 0 <= start <= end <= len(raw):
        raise ValueError('reference span outside subject')
    raw[:start].decode('utf-8')
    raw[:end].decode('utf-8')


def replacement_answer(answer):
    if answer.get('status') == 'exception':
        if classify_answer(answer) == 'refused':
            return dict(status='refused')
        raise ValueError('reference replacement execution failed: %r' % answer)
    if answer.get('status') != 'ok' or not isinstance(answer.get('hex'), str):
        raise ValueError('invalid reference replacement answer: %r' % answer)
    body = answer['hex']
    if re.fullmatch(r'(?:[0-9a-fA-F]{2})*', body) is None:
        raise ValueError('invalid reference replacement hex')
    return dict(status='ok', text=bytes.fromhex(body).decode('utf-8'))


def decode(answers, decoder):
    try:
        return Answers([decoder(answer) for answer in answers], answers.evidence)
    except (ValueError, UnicodeError) as error:
        answers.evidence['complete'] = False
        answers.evidence['error'] = str(error)
        raise Failure(str(error), answers.evidence) from error
