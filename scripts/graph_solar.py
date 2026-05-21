#!/usr/bin/env python3
"""Plot solar panel readings from SQLite database produced by poll_solar.py."""
import argparse
import datetime
import sqlite3
import sys

try:
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
except ImportError:
    sys.exit('matplotlib not installed — run: pip install matplotlib')

DEFAULT_DB = 'solar.db'


def load_rows(db_path, hours=None):
    conn = sqlite3.connect(db_path)
    if hours:
        since = (datetime.datetime.now() - datetime.timedelta(hours=hours)).isoformat()
        rows = conn.execute(
            'SELECT ts, voltage_mv, current_ma, power_mw FROM readings WHERE ts >= ? ORDER BY ts',
            (since,)
        ).fetchall()
    else:
        rows = conn.execute(
            'SELECT ts, voltage_mv, current_ma, power_mw FROM readings ORDER BY ts'
        ).fetchall()
    conn.close()
    return rows


def main():
    ap = argparse.ArgumentParser(description='Graph solar panel readings')
    ap.add_argument('--db',    default=DEFAULT_DB)
    ap.add_argument('--hours', type=float, default=None,
                    help='Show only the last N hours (default: all data)')
    args = ap.parse_args()

    rows = load_rows(args.db, args.hours)
    if not rows:
        sys.exit('No data in database.')

    times = [datetime.datetime.fromisoformat(r[0]) for r in rows]
    volts = [r[1] / 1000.0 for r in rows]   # mV → V
    amps  = [r[2]           for r in rows]   # mA
    watts = [r[3] / 1000.0  for r in rows]   # mW → W

    span = f'{times[0].strftime("%Y-%m-%d %H:%M")} – {times[-1].strftime("%Y-%m-%d %H:%M")}'

    fig, axes = plt.subplots(3, 1, figsize=(14, 8), sharex=True)
    fig.suptitle(f'Solar Panel Output  ({span})', fontsize=12)

    axes[0].plot(times, volts, color='steelblue', linewidth=1)
    axes[0].set_ylabel('Voltage (V)')
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(times, amps, color='seagreen', linewidth=1)
    axes[1].set_ylabel('Current (mA)')
    axes[1].grid(True, alpha=0.3)

    axes[2].plot(times, watts, color='tomato', linewidth=1)
    axes[2].set_ylabel('Power (W)')
    axes[2].grid(True, alpha=0.3)

    axes[2].xaxis.set_major_formatter(mdates.DateFormatter('%m/%d %H:%M'))
    plt.setp(axes[2].xaxis.get_majorticklabels(), rotation=45, ha='right')

    plt.tight_layout()
    plt.show()


if __name__ == '__main__':
    main()
