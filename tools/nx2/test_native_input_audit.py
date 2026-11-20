"""Synthetic frame boundaries; no game data or device access."""
import unittest

from native_input_audit import validate_input_window


def rows(counts=(7, 7, 7, 7, 7)):
    return [dict(frame=str(i), input_interference_count=str(count), input_audit_enabled='1')
            for i, count in enumerate(counts, 10)]


class AuditWindowTests(unittest.TestCase):
    def test_quiet_window_accepts_prior_manual_input(self):
        result = validate_input_window(rows(), 11, 15)
        self.assertTrue(result['valid_for_comparison'])
        self.assertEqual(result['interference_delta'], 0)

    def test_interference_at_first_interval_rejects(self):
        self.assertFalse(validate_input_window(rows((7, 8, 8, 8, 8)), 11, 15)['valid_for_comparison'])

    def test_interference_at_final_interval_rejects(self):
        self.assertFalse(validate_input_window(rows((7, 7, 7, 7, 8)), 11, 15)['valid_for_comparison'])

    def test_input_after_window_does_not_invalidate(self):
        data = rows() + [dict(frame='15', input_interference_count='8', input_audit_enabled='1')]
        self.assertTrue(validate_input_window(data, 11, 15)['valid_for_comparison'])

    def test_missing_previous_boundary_rejects(self):
        with self.assertRaisesRegex(ValueError, 'complete'):
            validate_input_window(rows()[1:], 11, 15)

    def test_gap_or_duplicate_rejects(self):
        for data in (rows()[:2] + rows()[3:], rows()[:2] + rows()[1:]):
            with self.assertRaisesRegex(ValueError, 'complete'):
                validate_input_window(data, 11, 15)

    def test_counter_reset_rejects(self):
        with self.assertRaisesRegex(ValueError, 'backward'):
            validate_input_window(rows((7, 8, 0, 7, 7)), 11, 15)

    def test_disabled_audit_rejects(self):
        data = rows()
        data[2]['input_audit_enabled'] = '0'
        with self.assertRaisesRegex(ValueError, 'enabled'):
            validate_input_window(data, 11, 15)

    def test_missing_counter_rejects(self):
        data = rows()
        del data[2]['input_interference_count']
        with self.assertRaisesRegex(ValueError, 'columns'):
            validate_input_window(data, 11, 15)


if __name__ == '__main__':
    unittest.main()
