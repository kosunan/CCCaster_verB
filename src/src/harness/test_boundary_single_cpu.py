from collections import defaultdict
import unittest
from run_boundary_single_cpu import validate


class SingleCpuTests(unittest.TestCase):
    def tables(self):
        tables = defaultdict(list)
        tables['BoundaryPool'] = [dict(capacity=0, launched=0, active=0)]
        tables['FrameStart'] = [dict(f=i, actual=i+1, exit=i+1, due=i) for i in range(1000)]
        tables['UpdateCadence'] = [dict(f=i, play=1, consecutive=1) for i in range(1000)]
        return tables

    def test_requires_zero_additional_threads_and_real_timestamp(self):
        text = '[GameCpuGuard] tid=1 process=1 before=1 chosen=0'
        self.assertTrue(validate(self.tables(), text)['passed'])
        for field in ('capacity', 'launched', 'active'):
            tables = self.tables()
            tables['BoundaryPool'][0][field] = 1
            with self.assertRaises(ValueError): validate(tables, text)
        tables = self.tables()
        tables['FrameStart'][0]['actual'] = 0
        with self.assertRaises(ValueError): validate(tables, text)
        with self.assertRaises(ValueError): validate(self.tables(), text.replace('process=1', 'process=ffff'))

    def test_shared_executor_pool_format(self):
        text = '[GameCpuGuard] tid=1 process=1 before=1 chosen=0'
        tables = self.tables()
        del tables['BoundaryPool'][0]['launched']
        self.assertTrue(validate(tables, text)['passed'])
        tables['BoundaryWorker'] = [dict(index=0, active=0)]
        with self.assertRaises(ValueError): validate(tables, text)
        tables['BoundaryWorker'] = []
        del tables['BoundaryPool'][0]['active']
        with self.assertRaises(ValueError): validate(tables, text)


if __name__ == '__main__':
    unittest.main()
