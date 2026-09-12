"""vgamepad 0.1.0を隔離環境へ配置する。setup.py/MSIは実行せず、既存ドライバを使用する。"""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request
import venv

ROOT = Path(__file__).resolve().parents[2]
VERSION = '0.1.0'


def main():
    target = ROOT / 'build/virtual-pad-venv'
    venv.EnvBuilder(with_pip=False).create(target)
    with urllib.request.urlopen(f'https://pypi.org/pypi/vgamepad/{VERSION}/json', timeout=30) as response:
        metadata = json.load(response)
    source = next(item for item in metadata['urls'] if item['packagetype'] == 'sdist')
    if not source['url'].startswith('https://files.pythonhosted.org/'):
        raise RuntimeError('予期しない配布元')
    with urllib.request.urlopen(source['url'], timeout=30) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != source['digests']['sha256']:
        raise RuntimeError('ソース配布物のSHA256不一致')
    package = target / 'Lib/site-packages'
    copied = []
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        prefix = f'vgamepad-{VERSION}/'
        for member in tar.getmembers():
            if not member.isfile() or not member.name.startswith(prefix + 'vgamepad/'):
                continue
            relative = Path(member.name[len(prefix):])
            if '..' in relative.parts or relative.is_absolute():
                raise RuntimeError('不正なアーカイブパス')
            # PythonモジュールとクライアントDLLのみ。ドライバ導入EXE/MSIを含めない。
            if relative.suffix != '.py' and relative.name != 'ViGEmClient.dll':
                continue
            destination = package / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(tar.extractfile(member).read())
            copied.append(str(relative))
    if not (package / 'vgamepad/__init__.py').exists():
        raise RuntimeError('ライブラリが見つからない')
    record = dict(version=VERSION, source=source['url'], sha256=source['digests']['sha256'],
                  driver_installer_executed=False, copied=copied)
    (target / 'virtual_controller_dependency.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    # import時に既存ViGEmBusへの接続を確認する。ドライバがなければ失敗する。
    subprocess.run([str(target / 'Scripts/python.exe'), '-c', 'import vgamepad; print("ViGEmBus connection OK")'], check=True)
    print(target)


if __name__ == '__main__':
    main()
