import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('agent',Path(__file__).resolve().parents[1]/'host/agent.py')
agent=importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent)

class MemoryTemperatureTests(unittest.TestCase):
    def read(self,stamp='100',status='ok',words='9766 10794 10537 10794 10794 10794 11308 11051',now=103):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'telemetry'
            p.write_text(f'BC250_MEMORY_V1\n{stamp}\n{status}\n{words}\n\n')
            return agent.read_memory_temperature(p,now)
    def test_known_chip_values_and_boundary(self):
        result=self.read(now=110)
        self.assertEqual(result,{'memory_temp_c':48,'memory_temp_avg_c':43.5})
    def test_stale_future_and_nonfinite_samples(self):
        for stamp,now in [('100',110.01),('104',103),('nan',103),('inf',103)]:
            self.assertIsNone(self.read(stamp=stamp,now=now)['memory_temp_c'])
    def test_errors_and_malformed_words_are_unavailable(self):
        for status,words in [('error',''),('stopped',''),('ok','1 2'),('ok','no data'),('ok',' '.join(['81']*8)),('ok',' '.join(['-1']*8)),('ok',' '.join([str(2**32)]*8))]:
            self.assertIsNone(self.read(status=status,words=words)['memory_temp_c'])
    def test_missing_snapshot_is_unavailable(self):
        self.assertIsNone(agent.read_memory_temperature(Path('/missing/bc250-test'),103)['memory_temp_c'])
    def test_zero_and_saturation_are_real_values(self):
        self.assertEqual(self.read(words=' '.join(['20']*8))['memory_temp_c'],0)
        self.assertEqual(self.read(words=' '.join(['80']*8))['memory_temp_c'],120)

if __name__=='__main__':unittest.main()
