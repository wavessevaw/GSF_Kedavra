"""Compare scheduler builds on the same latches, analysis completions and display ticks."""
import argparse
import io
import subprocess

import numpy as np


def replay(binary, hz, fps, scenario):
    data = subprocess.check_output([binary, str(hz), str(fps), str(scenario)])
    return np.loadtxt(io.BytesIO(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    args = parser.parse_args()
    for hz in (60, 90):
        for fps, scenario in [(n, 0) for n in (24, 27, 28, 29, 29.7, 30, 31, 32)] + [(30, 1), (28, 2)]:
            old, new = [replay(b, hz, fps, scenario) for b in (args.baseline, args.candidate)]
            # Same flip, output kind, real sources and pair on every single decision.
            np.testing.assert_array_equal(old[:, [0, 2, 5, 6]], new[:, [0, 2, 5, 6]])
            assert np.min(np.diff(new[:, 1])) >= -1e-7
            midpoint = (old[:, 2] == 2) & (2 * old[:, 3] == old[:, 4])
            np.testing.assert_array_equal(old[midpoint], new[midpoint])
            old_error, new_error = [np.abs(np.diff(rows[:, 1]) - 1) for rows in (old, new)]
            rms = [np.sqrt(np.mean(e**2)) for e in (old_error, new_error)]
            if not scenario and fps == 30:
                np.testing.assert_array_equal(old, new)
            if not scenario and fps == 28:
                assert rms[1] < rms[0]
            print(hz, fps, scenario, "RMS", *rms, "P95", *[np.percentile(e, 95) for e in (old_error, new_error)])


if __name__ == "__main__":
    main()
