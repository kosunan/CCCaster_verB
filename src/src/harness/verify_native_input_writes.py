"""Verify native store observations and their subsequent rollback corrections."""
import re


def verify_local_disabled(text):
    active = re.findall(r'\[(?:LocalInputRollback|NativeInputWrite)\] ACTIVE[^\n]*', text)
    replays = re.findall(r'\[(?:Rollback|LocalInputRollback)\] (?:BEGIN|END)[^\n]*', text)
    initialized = '[SceneRunner] rollback session initialized' in text
    return dict(passed=initialized and not active and not replays,
                initialized=initialized, activations=len(active), replays=len(replays))


def verify(text, minimum=120, require_corrections=True, players=(1, 2)):
    pending = {}
    observed = {}
    writes = [0, 0]
    corrections = [0, 0]
    mismatches = [0, 0]
    errors = []
    confirmed = max((int(n) for n in re.findall(r'\[CONFIRMED\] (\d+)', text)), default=0)
    for line in text.splitlines():
        match = re.search(r'\[NativeInputWrite\] (WRITE|MISMATCH|CORRECTED) (.*)', line)
        if not match:
            continue
        kind, body = match.groups()
        fields = dict(re.findall(r'(\w+)=(\w+)', body))
        player = int(fields['player']) - 1
        address = int(fields['address'], 16)
        # Normal actors and the two native partner slots use the same captured input source.
        if player not in (0, 1) or address not in tuple(0x55541B+i*0xAFC for i in range(4)):
            errors.append('invalid actor address or player')
            continue
        key = (int(fields['frame']), player, address, fields['raw'])
        if kind == 'WRITE':
            writes[player] += 1
            observed[key] = (fields['direction'], fields['buttons'], fields['released'])
            if fields.get('equal') != '1':
                errors.append('native store differs from captured conversion conditions')
        elif kind == 'MISMATCH':
            mismatches[player] += 1
            pending[key] = fields
        else:
            values = (fields['direction'], fields['buttons'], fields['released'])
            if observed.get(key) != values:
                errors.append('correction is not backed by the native memory read')
            if key not in pending:
                errors.append('correction without a preceding address mismatch')
            else:
                del pending[key]
                corrections[player] += 1
    missing = [key for key in pending if key[0] <= confirmed]
    passed = not errors and not missing and all(writes[p-1] >= minimum for p in players)
    if require_corrections:
        passed &= all(corrections[p-1] > 0 for p in players)
    else:
        passed &= sum(corrections) > 0
    return dict(passed=passed, writes=writes, mismatches=mismatches, corrections=corrections,
                missing_confirmed=missing[:10], missing_count=len(missing), errors=errors[:10])


def verify_local(text, mode, players=(1, 2), minimum=120):
    result = verify(text, minimum, players=players)
    active = re.findall(r'\[LocalInputRollback\] ACTIVE mode=(\d+)', text)
    ends = re.findall(r'\[LocalInputRollback\] END mode=(\d+) target=\d+ world=(\d+) expectedWorld=(\d+)', text)
    matching = [(int(world), int(expected)) for m, world, expected in ends if int(m) == mode]
    result['activations'] = active.count(str(mode))
    result['replays'] = len(matching)
    result['time_mismatches'] = sum(world != expected for world, expected in matching)
    result['passed'] &= bool(result['activations'] and matching) and not result['time_mismatches']
    return result
