#!/usr/bin/env python3
"""Show local artifacts and the latest native test report without running the game."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, help='Optional JSON report to inspect')
    args = parser.parse_args()
    artifacts = {
        'Program natywny': ROOT / 'build/native/ps2xRuntime/ratchet-recomp',
        'Obraz płyty': ROOT / '.local/disc/Ratchet-and-Clank-3-SCES-52456.iso',
        'Program startowy ELF': ROOT / '.local/input/rac3-pal-boot.elf',
    }
    for label, path in artifacts.items():
        print(f'{label}: {"dostępny" if path.is_file() else "brak"}')
    if args.report:
        report_path = args.report
    else:
        candidates = []
        for path in (ROOT / '.local/reports').glob('native-*.json'):
            try:
                candidate = json.loads(path.read_text())
                if isinstance(candidate, dict) and 'binary' in candidate and 'gameplay_validated' in candidate:
                    candidates.append(path)
            except (OSError, ValueError):
                continue
        if not candidates:
            print('Brak raportu testu. Uruchom: python3 tools/smoke-native.py')
            return 1
        report_path = max(candidates, key=lambda path: path.stat().st_mtime_ns)
    try:
        report = json.loads(report_path.read_text())
        if not isinstance(report, dict):
            raise ValueError('report must be a JSON object')
    except (OSError, ValueError) as error:
        print(f'Nie można odczytać raportu: {error}')
        return 1
    print(f'Raport: {report_path}')
    print('Rozgrywka: ' + ('potwierdzona w raporcie' if report.get('gameplay_validated')
                          else 'jeszcze niepotwierdzona'))
    for item in report.get('missing_iop_imports', []):
        print(f'Brak obsługi IOP: {item["library"]}:{item["ordinal"]} (wersja {item["version"]})')
    for sid in report.get('pending_rpc_bindings', []):
        print(f'Oczekiwanie na usługę RPC: {sid}')
    if report.get('hard_timeout'):
        print('Test przekroczył limit czasu i został przerwany.')
    if report.get('state'):
        print(report['state'])
    for blocker in report.get('blockers', []):
        print(f'Blokada: {blocker.split(" trace=", 1)[0]}')
    print('To zapis poprzedniego testu; polecenie nie uruchamia gry.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
