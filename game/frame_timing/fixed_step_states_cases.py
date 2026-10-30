#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Edge cases for the fixed-step state methods (fixed_step_states.cpp), for tools/verify.py run.

usage: fixed_step_states_cases.py CAPTURES_DIR

Writes CAPTURES_DIR/<function>/edge/*.json for all fourteen methods. The owner's advance method
(vtable slot +0xd0) is a stand-in game function the case stubs; its descriptor is compared, and
it leaves the busy count (and for the gate, the state) the case says. Covers idle and busy owners,
staying busy after the advance, and states around the 2..5 range of request_7 (with wraparound
values). For SprjDungeonGateIns: every mode and values past them, events with the wrong id or
missing one of the two flags, and advances that change the state between event 0's two requests;
the gate's set_step and method_38 and the step counter are stubbed and compared.
"""
import json
import os
import struct
import sys

ADVANCE = '0x00400020'
METHODS = [  # (address, kind, owner field)
    ('0x01f6a9f0', 'until_idle', 8), ('0x0200d8a0', 'until_idle', 16), ('0x0200e100', 'until_idle', 8),
    ('0x0200e270', 'request_7', 8), ('0x02012610', 'until_idle', 8), ('0x02012780', 'request_10', 8),
    ('0x020128f0', 'until_idle', 8), ('0x02012a60', 'request_7', 8), ('0x02012bf0', 'until_idle', 8),
]


GATE_SET_STEP, GATE_METHOD_38, STEP_COUNTER = '0x00400040', '0x00400060', '0x01ec3000'
GATE_METHODS = [  # (address, name)
    ('0x01eb5390', 'dungeon_gate_step_init_for_channel_load'), ('0x01eb5490', 'dungeon_gate_step_mode_switch'),
    ('0x01eb9020', 'dungeon_gate_on_event_0'), ('0x01ebb090', 'dungeon_gate_on_event_1000'),
    ('0x01ebf060', 'dungeon_gate_on_event_3000'),
]
GATE_EVENT_IDS = {'0x01eb9020': 0, '0x01ebb090': 1000, '0x01ebf060': 3000}
OWNER = 0x108  # the state owner inside the gate
# Owner fields (offsets from the owner) and gate fields (from the gate), as fixed_step_states.cpp
# declares them.
STATE, REQUEST, BUSY, VALUE_26C = 0x25c, 0x260, 0x264, 0x26c
REQUEST_TAKEN, VALUE_6A0 = 0x378, 0x6a0
GATE_EVENT_FLAGS = 0x1400  # both flags a gate event must carry


def u32(v):
    return struct.pack('<I', v & 0xffffffff).hex()


def case(address, kind, field, cid, busy, busy_after=0, state=3, request=0x55):
    c = {'schema': 1, 'address': address, 'id': cid, 'returns': 'void' if kind == 'request_10' else 'i32',
         'args': {'rdi': 'buf:self'},
         'buffers': {'self': {'size': 24}, 'owner': {'size': 616}, 'owner_vt': {'size': 216}},
         'memory': [{'addr': f'buf:self+{field}', 'pointer': 'owner'}, {'addr': 'buf:owner', 'pointer': 'owner_vt'},
                    {'addr': 'buf:owner+604', 'bytes': u32(state)}, {'addr': 'buf:owner+608', 'bytes': u32(request)},
                    {'addr': 'buf:owner+612', 'bytes': u32(busy)}, {'addr': 'buf:owner_vt+208', 'guest': ADVANCE}],
         'stubs': [{'address': ADVANCE, 'argc': 2, 'argi': [0, 1], 'argmem': [{'arg': 1, 'size': 12}, {'arg': 0, 'offset': 604, 'size': 12}],
                    'writes': [{'arg': 0, 'offset': 612, 'bytes': u32(busy_after)}]}],
         'imports': []}
    return c


def cases_for(address, kind, field):
    out = [case(address, kind, field, 'idle', 0), case(address, kind, field, 'busy_then_idle', 1, 0),
           case(address, kind, field, 'busy_stays_busy', 1, 1), case(address, kind, field, 'busy_negative', -1, 0),
           case(address, kind, field, 'idle_becomes_busy', 0, 5)]
    if kind == 'request_7':
        for s in (0, 1, 2, 5, 6, 0xffffffff, 0x80000002):
            out.append(case(address, kind, field, f'idle_state_{s:x}', 0, 0, state=s))
    return out


def gate_case(address, cid, busy, state, advances=(), event_id=None, flags=GATE_EVENT_FLAGS, value_26c=0x7fc00001, rdx=None):
    """advances: (busy, state) the owner is left with by each call of its advance method."""
    owner_at = lambda off: f'buf:gate+{OWNER + off}'
    c = {'schema': 1, 'address': address, 'id': cid, 'returns': 'void', 'args': {'rdi': 'buf:gate'},
         'buffers': {'gate': {'size': 0x6b0}, 'gate_vt': {'size': 0x40}, 'owner_vt': {'size': 216}, 'event': {'size': 0x20}},
         'memory': [{'addr': 'buf:gate', 'pointer': 'gate_vt'}, {'addr': owner_at(0), 'pointer': 'owner_vt'},
                    {'addr': owner_at(STATE), 'bytes': u32(state)}, {'addr': owner_at(REQUEST), 'bytes': u32(0x55)},
                    {'addr': owner_at(BUSY), 'bytes': u32(busy)}, {'addr': owner_at(VALUE_26C), 'bytes': u32(value_26c)},
                    {'addr': f'buf:gate+{REQUEST_TAKEN}', 'bytes': '5a'},
                    {'addr': f'buf:gate+{VALUE_6A0}', 'bytes': u32(0x12345678)},
                    {'addr': 'buf:owner_vt+208', 'guest': ADVANCE}, {'addr': 'buf:gate_vt+40', 'guest': GATE_SET_STEP},
                    {'addr': 'buf:gate_vt+56', 'guest': GATE_METHOD_38},
                    {'addr': 'buf:event+24', 'bytes': struct.pack('<Q', flags).hex()}],
         'stubs': [{'address': ADVANCE, 'argc': 2, 'argi': [0, 1],
                    # The owner's state, request, busy, value_26c and the gate's request_taken right
                    # after them, and value_6a0: what the owner sees while it advances.
                    'argmem': [{'arg': 1, 'size': 12}, {'arg': 0, 'offset': STATE, 'size': REQUEST_TAKEN - OWNER - STATE + 1},
                               {'arg': 0, 'offset': VALUE_6A0 - OWNER, 'size': 4}],
                    'writes': [{'arg': 0, 'offset': STATE, 'bytes': u32(s_)}, {'arg': 0, 'offset': BUSY, 'bytes': u32(b_)}]}
                   for b_, s_ in advances] +
                  [{'address': GATE_SET_STEP, 'argc': 2, 'argi': [0, 1]}, {'address': GATE_METHOD_38, 'argc': 1},
                   {'address': STEP_COUNTER, 'argc': 1}],
         'imports': []}
    if event_id is not None:
        c['args'].update({'rsi': 'buf:event', 'rdx': (event_id & 0xffffffff) if rdx is None else rdx})
    return c


def gate_cases(address):
    out = []
    if address == '0x01eb5390':  # STEP_Init_forChannelLoad
        out += [gate_case(address, 'channel_load_idle', 0, 0, [(0, 0)]),
                gate_case(address, 'channel_load_idle_stays_busy', 0, 0, [(3, 4)]),
                gate_case(address, 'busy', 1, 0), gate_case(address, 'busy_negative', -1, 0)]
        out += [gate_case(address, f'idle_state_{s:x}', 0, s) for s in (1, 6, 7, 0xffffffff)]
    elif address == '0x01eb5490':  # STEP_ModeSwitch
        out += [gate_case(address, f'idle_mode_{s:x}', 0, s) for s in (0, 1, 2, 3, 4, 5, 6, 7, 0xffffffff, 0x80000000)]
        out += [gate_case(address, 'busy_then_idle_mode_3', 1, 0, [(0, 3)]),
                gate_case(address, 'busy_then_idle_mode_6', 2, 5, [(0, 6)]),
                gate_case(address, 'busy_then_idle_mode_9', 1, 0, [(0, 9)]),
                gate_case(address, 'busy_stays_busy', 1, 2, [(1, 2)]),
                gate_case(address, 'busy_negative_stays', -1, 2, [(-1, 4)])]
    else:
        wanted = GATE_EVENT_IDS[address]
        ev = lambda cid, busy, state, adv=(), event_id=wanted, flags=GATE_EVENT_FLAGS, rdx=None: gate_case(
            address, cid, busy, state, adv, event_id, flags, rdx=rdx)
        out += [ev('wrong_id', 0, 0, event_id=wanted + 1), ev('wrong_id_negative', 0, 0, event_id=-1),
                ev('flag_400_only', 0, 0, flags=0x400), ev('flag_1000_only', 0, 0, flags=0x1000),
                ev('no_flags', 0, 0, flags=0), ev('extra_flags', 0, 3, [(0, 3), (0, 3)], flags=0xffffffffffffffff),
                ev('busy', 1, 3), ev('busy_negative', -1, 0)]
        out += [ev(f'idle_state_{s:x}', 0, s, [(0, s), (0, s)]) for s in (0, 1, 2, 3, 4, 5, 6, 7, 0xffffffff, 0x80000002)]
        # The id is compared as 32 bits: the upper half of rdx does not count.
        out += [ev('id_upper_bits_set', 0, 3, [(0, 3), (0, 3)], rdx=(1 << 32) | wanted),
                ev('id_upper_bits_only', 0, 3, [(0, 3), (0, 3)], rdx=(1 << 32) | ((wanted + 1) & 0xffffffff))]
        # The request is reported taken even when the advance leaves the owner busy or moves it on.
        out += [ev('advance_leaves_busy', 0, 3, [(4, 3), (4, 3)]), ev('advance_changes_state', 0, 2, [(0, 6), (0, 6)])]
        if address == '0x01eb9020':  # request 1, then request 2 sees what the first advance left
            out += [ev('request_1_then_channel_load', 0, 3, [(0, 0), (0, 0)]),
                    ev('request_1_leaves_busy', 0, 3, [(2, 0)]),
                    ev('request_1_to_error', 0, 4, [(0, 6)])]
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    n = 0
    for address, kind, field in METHODS:
        out = os.path.join(sys.argv[1], f'frame_timing_{kind}_{address[2:]}', 'edge')
        os.makedirs(out, exist_ok=True)
        for c in cases_for(address, kind, field):
            with open(os.path.join(out, f"{c['id']}.json"), 'w') as f:
                json.dump(c, f, indent=1)
            n += 1
    for address, name in GATE_METHODS:
        out = os.path.join(sys.argv[1], f'frame_timing_{name}', 'edge')
        os.makedirs(out, exist_ok=True)
        for c in gate_cases(address):
            with open(os.path.join(out, f"{c['id']}.json"), 'w') as f:
                json.dump(c, f, indent=1)
            n += 1
    print(f'{n} cases written under {sys.argv[1]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
