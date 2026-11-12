# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from recovery_report import recovery_report


def pair(i, decision, dispatched=0, accepted=0):
    return dict(event='pair', pid=1, generation=1, source1_id=i+1,
                admission_decision=decision, dispatched_outputs=dispatched,
                wsi_accepted_ns=[accepted, 0, 1])


class RecoveryReportTest(unittest.TestCase):
    def report(self, rows):
        return recovery_report([pair(0, 'budget_blocked')]+rows, 1, 1)

    def test_stays_blocked(self):
        r=self.report([pair(1, 'budget_blocked')])
        self.assertEqual((r['status'], r['admission_exits']), ('blocked_after_release', 0))

    def test_exits_without_dispatch(self):
        r=self.report([pair(1, 'admitted')])
        self.assertEqual((r['status'], r['admission_exits']), ('admitted_no_dispatch', 1))

    def test_dispatch_is_not_display(self):
        self.assertEqual(self.report([pair(1, 'admitted', 1)])['status'], 'dispatched_no_wsi')

    def test_reentries(self):
        r=self.report([pair(1, 'admitted', 1, 1), pair(2, 'deadline_blocked'),
                       pair(3, 'admitted', 1), pair(4, 'budget_blocked')])
        self.assertEqual((r['status'], r['readmission_reentries']), ('unstable_readmission', 2))

    def test_recovered(self):
        self.assertEqual(self.report([pair(1, 'history_refresh'), pair(2, 'admitted', 1, 1)])['status'], 'recovered')

    def test_legacy_is_unknown(self):
        p=pair(1, 'admitted', 1, 1);del p['admission_decision']
        self.assertIsNone(self.report([p])['admission_exits'])

    def test_empty_is_unknown(self):
        self.assertEqual(recovery_report([], 1, 1)['status'], 'insufficient_evidence')


if __name__ == '__main__':
    unittest.main()
