"""Portable checks of result mapping only, not Linux isolation evidence."""
import copy
import unittest
from oj_integration import judge


class JudgeTests(unittest.TestCase):
    def setUp(self):
        stream = dict(eof=True, flushed=True, discarded_bytes=0, failed_bytes=0, write_error='')
        self.report = dict(isolated=True, summary='success', exit_code=0,
                           stdout=copy.copy(stream), stderr=copy.copy(stream), errors=[],
                           cleanup=dict(descendants_empty=True, launcher_reaped=True,
                                        cgroup_removed=True, errors=[]))

    def test_answer_is_separate_from_exit(self):
        self.assertEqual(judge(self.report, b'5\n', b'5'), 'ACCEPTED')
        self.assertEqual(judge(self.report, b'6\n', b'5'), 'WRONG_ANSWER')

    def test_truncated_matching_prefix_is_not_accepted(self):
        self.report['stdout']['discarded_bytes'] = 12
        self.assertEqual(judge(self.report, b'5', b'5'), 'OUTPUT_LIMIT')

    def test_infrastructure_errors_override_answer(self):
        mutations = [('isolated', False), ('summary', 'startup_failure'),
                     ('verdict', 'cleanup_incomplete'), ('errors', ['failure'])]
        for key, value in mutations:
            with self.subTest(key=key):
                report = copy.deepcopy(self.report)
                report[key] = value
                self.assertEqual(judge(report, b'5', b'5'), 'SYSTEM_ERROR')

    def test_incomplete_cleanup_or_output_is_not_accepted(self):
        for group, key in [('cleanup', 'descendants_empty'), ('cleanup', 'launcher_reaped'),
                           ('cleanup', 'cgroup_removed'), ('stdout', 'eof'), ('stderr', 'flushed')]:
            with self.subTest(group=group, key=key):
                report = copy.deepcopy(self.report)
                report[group][key] = False
                self.assertEqual(judge(report, b'5', b'5'), 'SYSTEM_ERROR')

    def test_execution_failures(self):
        for summary, expected in [('timeout', 'TIME_LIMIT'), ('oom', 'MEMORY_LIMIT'),
                                  ('signaled', 'RUNTIME_ERROR'), ('nonzero_exit', 'RUNTIME_ERROR')]:
            with self.subTest(summary=summary):
                self.report['summary'] = summary
                self.assertEqual(judge(self.report, b'', b''), expected)


if __name__ == '__main__':
    unittest.main()
