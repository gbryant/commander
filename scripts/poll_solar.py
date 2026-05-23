#!/usr/bin/env python3
"""Poll INA219 readings from commander via telnet and store in SQLite."""
import argparse
import datetime
import re
import socket
import sqlite3
import sys
import time

DEFAULT_HOST     = 'esp32.local'
DEFAULT_PORT     = 23
DEFAULT_DB       = 'solar.db'
DEFAULT_INTERVAL = 60  # seconds


def strip_iac(data: bytes) -> bytes:
    """Remove RFC 854 telnet IAC negotiation sequences."""
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] == 0xFF and i + 1 < len(data):
            i += 3 if data[i + 1] >= 0xFB else 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def read_until_prompt(sock, timeout=5.0):
    """Accumulate bytes until '> ' prompt, stripping IAC sequences."""
    sock.settimeout(timeout)
    raw = b''
    while True:
        chunk = sock.recv(256)
        if not chunk:
            raise ConnectionError('connection closed')
        raw += chunk
        cleaned = strip_iac(raw)
        if cleaned.endswith(b'> '):
            return cleaned.decode('utf-8', errors='ignore')


def run_command(sock, cmd):
    """Send a command and return the response line (strips echo and prompt)."""
    sock.sendall((cmd + '\r\n').encode())
    response = read_until_prompt(sock)
    # response: "cmd\r\nvalue unit\r\n> "
    lines = [l.strip() for l in response.replace('\r\n', '\n').split('\n')]
    lines = [l for l in lines if l and l != cmd and l != '>']
    return lines[0] if lines else None


def parse_float(s):
    if not s:
        return None
    m = re.match(r'^(-?[\d.]+)', s)
    return float(m.group(1)) if m else None


def init_db(path):
    conn = sqlite3.connect(path)
    conn.execute('''
        CREATE TABLE IF NOT EXISTS readings (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            ts         TEXT NOT NULL,
            voltage_mv REAL,
            current_ma REAL,
            power_mw   REAL
        )
    ''')
    conn.commit()
    return conn


def connect(host, port):
    sock = socket.create_connection((host, port), timeout=10)
    read_until_prompt(sock)  # consume greeting + initial prompt
    return sock


def poll_once(sock):
    # astats reads voltage and current in a single round-trip (~1 ms between
    # the two I2C reads) so both values come from the same ADC cycle.
    line = run_command(sock, 'astats')
    if not line or ',' not in line:
        return None, None, None
    try:
        parts   = line.split(',')
        voltage = float(parts[0])
        current = float(parts[1])
    except (ValueError, IndexError):
        return None, None, None
    power = voltage * current / 1000.0
    return voltage, current, power


def main():
    ap = argparse.ArgumentParser(description='Poll solar readings via telnet')
    ap.add_argument('--host',     default=DEFAULT_HOST)
    ap.add_argument('--port',     default=DEFAULT_PORT, type=int)
    ap.add_argument('--db',       default=DEFAULT_DB)
    ap.add_argument('--interval', default=DEFAULT_INTERVAL, type=int,
                    help='seconds between polls (default: 60)')
    ap.add_argument('--prefix',   default='a',
                    help='INA219 command prefix (default: a)')
    args = ap.parse_args()

    db   = init_db(args.db)
    sock        = None
    error_count = 0   # consecutive parse errors
    reinit_sent = False
    retry_delay = 10  # seconds; doubles on each failure, caps at 300

    print(f'Polling {args.host}:{args.port} every {args.interval}s → {args.db}')
    print('Ctrl-C to stop.\n')

    while True:
        try:
            if sock is None:
                print(f'Connecting to {args.host}...', end=' ', flush=True)
                sock = connect(args.host, args.port)
                print('connected.')
                retry_delay = 10
                error_count = 0
                reinit_sent = False

            voltage, current, power = poll_once(sock)
            ts = datetime.datetime.now().isoformat(timespec='seconds')

            if voltage is None:
                error_count += 1
                print(f'{ts}  parse error — skipping')
                if error_count >= 3:
                    run_command(sock, f'{args.prefix}init')
                    print(f'{ts}  sent {args.prefix}init (repeated errors)')
                    error_count = 0
            else:
                error_count = 0
                if current == 0.0 and voltage > 1000 and not reinit_sent:
                    run_command(sock, f'{args.prefix}init')
                    print(f'{ts}  0.0 mA — sent {args.prefix}init')
                    reinit_sent = True
                elif current != 0.0:
                    reinit_sent = False
                db.execute(
                    'INSERT INTO readings (ts, voltage_mv, current_ma, power_mw) VALUES (?,?,?,?)',
                    (ts, voltage, current, power)
                )
                db.commit()
                power_w = f'{power/1000:.3f} W' if power is not None else '--- W'
                print(f'{ts}  {voltage/1000:.3f} V  {current:7.1f} mA  {power_w}')

        except KeyboardInterrupt:
            print('\nStopped.')
            sys.exit(0)

        except (OSError, ConnectionError) as e:
            ts = datetime.datetime.now().isoformat(timespec='seconds')
            print(f'{ts}  {e} — retrying in {retry_delay}s')
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
            sock = None
            time.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 300)
            continue

        time.sleep(args.interval)


if __name__ == '__main__':
    main()
