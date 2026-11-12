#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Classify observed admission recovery; GSF 0.1.7 has no COOLDOWN enum."""
from collections import Counter


def recovery_report(events, release_frame, recovery_start):
    pairs = [e for e in events if e.get('event') == 'pair']
    if len({(p['pid'], p['generation']) for p in pairs}) > 1:
        raise ValueError('Recovery requires one process/bridge scope')
    if not pairs or any('admission_decision' not in p for p in pairs):
        return {'status': 'insufficient_evidence', 'reason': 'explicit_admission_decisions_missing',
                'admission_exits': None, 'readmission_reentries': None}
    pairs.sort(key=lambda p: p['source1_id'])
    blocked = {'budget_blocked', 'deadline_blocked'}
    previous = None
    exits = reentries = 0
    after = []
    for p in pairs:
        decision = p['admission_decision']
        released = p['source1_id']-1 >= release_frame
        if released:
            after.append(p)
        # History refresh / pressure / unplanned source slots are distinct
        # from a budget/deadline block. Do not invent COOLDOWN transitions.
        if decision not in blocked | {'admitted'}:
            continue
        if released:
            exits += decision == 'admitted' and previous in blocked
            reentries += decision in blocked and previous == 'admitted'
        previous = decision
    admitted = [p for p in after if p['admission_decision'] == 'admitted']
    dispatched = [p for p in after if p['dispatched_outputs']]
    accepted = [p for p in after if p['wsi_accepted_ns'][0]]
    stable_accepted = [p for p in accepted if p['source1_id']-1 >= recovery_start]
    status = ('blocked_after_release' if not admitted and any(p['admission_decision'] in blocked for p in after)
              else 'insufficient_evidence' if not admitted
              else 'admitted_no_dispatch' if not dispatched
              else 'unstable_readmission' if reentries >= 2
              else 'dispatched_no_wsi' if not stable_accepted
              else 'recovered')
    return {'status': status, 'scope': 'observed_admission_not_COOLDOWN_state_or_scanout',
            'admission_exits': exits, 'readmission_reentries': reentries,
            'admitted_pairs_after_release': len(admitted),
            'dispatched_pairs_after_release': len(dispatched),
            'accepted_pairs_after_release': len(accepted),
            'accepted_pairs_in_recovery_window': len(stable_accepted),
            'decisions_after_release': dict(Counter(p['admission_decision'] for p in after)),
            'first_admitted_source_id': admitted[0]['source1_id'] if admitted else None,
            'first_dispatched_source_id': dispatched[0]['source1_id'] if dispatched else None,
            'first_accepted_source_id': accepted[0]['source1_id'] if accepted else None}
