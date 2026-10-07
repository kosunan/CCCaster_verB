"""日本語・空白を含む配置先からCLIを起動し、引数検査まで到達することを確認する。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == 'nt', 'Windows CLI startup')
class CliUnicodeStartupTest(unittest.TestCase):
    def test_unicode_executable_path(self):
        root = Path(__file__).resolve().parents[3]
        binary = root / 'build/bin/CCCaster_B.exe'
        self.assertTrue(binary.is_file(), '先にルートbuild.batを実行してください')
        with tempfile.TemporaryDirectory(prefix='cccaster_cli_') as temp:
            folder = Path(temp) / '日本語 メルブラ' / 'cccaster_B'
            folder.mkdir(parents=True)
            exe = folder / binary.name
            shutil.copy2(binary, exe)
            # 実ゲームを開始せず、設定読込みの後にあるポート引数検査で終了する。
            config = folder / 'cccaster.ini'
            original = b'[Netplay]\nDefaultDelay=2\n'
            config.write_bytes(original)
            for cwd in (folder, Path(temp)):
                with self.subTest(cwd=str(cwd)):
                    result = subprocess.run([str(exe), '--port', '0'], cwd=cwd,
                                            capture_output=True, timeout=10,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                    self.assertEqual(result.returncode, 2, result.stderr.decode('utf-8', 'replace'))
                    self.assertIn('ポート番号は1〜65535', result.stderr.decode('utf-8'))
                    self.assertEqual(config.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
