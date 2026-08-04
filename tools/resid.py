#!/usr/bin/env python3
"""Compare the captured summing-bus output against the sum of the captured pass-throughs.

Using the pass-throughs rather than the generators as the reference is the point: they are
what the senders actually put on the bus, so anything left over is the bus's own doing and
not the device's, the host's, or the generator's.

Channel order in the WAV is the --cap order: 1, 9, 10, 11, 12, 13.
"""
import sys
import struct
import numpy as np


def read_wav_f32(path):
    """Minimal RIFF reader. Python's `wave` rejects format tag 3 (IEEE float), which is
    exactly what we write, so parse the chunks directly."""
    raw = open(path, 'rb').read()
    assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE', 'not a RIFF/WAVE file'
    pos, nch, sr, fmt = 12, None, None, None
    data = None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos+4]
        (csz,) = struct.unpack('<I', raw[pos+4:pos+8])
        body = raw[pos+8:pos+8+csz]
        if cid == b'fmt ':
            fmt, nch, sr = struct.unpack('<HHI', body[:8])
        elif cid == b'data':
            data = body
        pos += 8 + csz + (csz & 1)
    assert fmt == 3, f'expected IEEE float (3), got {fmt}'
    return np.frombuffer(data, dtype='<f4').reshape(-1, nch), sr


def db(x):
    x = float(x)
    return 20 * np.log10(x) if x > 1e-20 else -240.0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'sum1.wav'
    data, sr = read_wav_f32(path)
    print(f"{path}: {data.shape[0]} frames x {data.shape[1]} ch @ {sr} Hz\n")

    # Optional: resid.py <file> <sum-col> <sender-col>...  (0-based columns in the WAV,
    # i.e. positions in the --cap list). Defaults match the 1,9,10,11,12,13 capture.
    if len(sys.argv) > 3:
        sum_col = int(sys.argv[2])
        send_cols = [int(a) for a in sys.argv[3:]]
    else:
        sum_col, send_cols = 5, [1, 2, 3, 4]

    labels = send_cols
    senders = data[:, send_cols]
    summed  = data[:, sum_col]

    expected_undelayed = senders.sum(axis=1)

    # Find the delay between the sum channel and the sum of the senders.
    best = None
    for d in range(0, 2049):
        a = summed[d:]
        b = expected_undelayed[:len(a)]
        num = float(np.dot(a, b))
        den = float(np.linalg.norm(a) * np.linalg.norm(b)) + 1e-30
        c = num / den
        if best is None or c > best[1]:
            best = (d, c)
    delay, corr = best
    print(f"best alignment: sum lags senders by {delay} samples (corr {corr:.6f})\n")

    a = summed[delay:]
    S = senders[:len(a), :]

    # Solve for the gain each sender contributes to the sum. This is what actually verifies
    # trim and mute: a muted sender must come out at exactly zero, and -6 dB of trim must
    # come out at 0.5012, with no residual left over either way.
    g, *_ = np.linalg.lstsq(S, a, rcond=None)
    print("per-sender gain recovered from the sum:")
    for i, gi in enumerate(g):
        print(f"  sender col {labels[i]:2d}: {gi: .6f}  ({db(abs(gi)):7.2f} dB)")
    print()

    b = S @ g
    r = a - b

    print(f"sum        rms {db(np.sqrt(np.mean(a**2))):8.1f} dBFS   peak {db(np.max(np.abs(a))):8.1f} dBFS")
    print(f"expected   rms {db(np.sqrt(np.mean(b**2))):8.1f} dBFS   peak {db(np.max(np.abs(b))):8.1f} dBFS")
    print(f"residual   rms {db(np.sqrt(np.mean(r**2))):8.1f} dBFS   peak {db(np.max(np.abs(r))):8.1f} dBFS")
    print(f"residual is {db(np.sqrt(np.mean(r**2))) - db(np.sqrt(np.mean(b**2))):.1f} dB below the expected sum\n")

    # Where is the residual energy? If the bus is misbehaving at block boundaries this will
    # show up as a comb of spikes at a fixed period.
    nz = np.flatnonzero(np.abs(r) > 1e-6)
    print(f"samples with |residual| > 1e-6: {len(nz)} of {len(r)} ({100.0*len(nz)/len(r):.2f}%)")
    if len(nz):
        print(f"first 24 such sample indices: {nz[:24]}")
        d = np.diff(nz)
        if len(d):
            vals, counts = np.unique(d, return_counts=True)
            order = np.argsort(-counts)[:8]
            print("most common gaps between them:",
                  ", ".join(f"{vals[i]}x{counts[i]}" for i in order))
        # Block-phase histogram for a few candidate block sizes.
        for blk in (64, 128, 256, 512, 1024, 2048):
            ph = nz % blk
            frac_at_zero = float(np.mean(ph == 0))
            print(f"  block {blk:5d}: {100*frac_at_zero:6.2f}% of residual samples sit exactly on a boundary"
                  f" (distinct phases seen: {len(np.unique(ph))})")

    # Per-block residual energy, to see whether whole blocks are wrong rather than samples.
    blk = 256
    nblk = len(r) // blk
    be = np.sqrt(np.mean(r[:nblk*blk].reshape(nblk, blk)**2, axis=1))
    ge = np.sqrt(np.mean(b[:nblk*blk].reshape(nblk, blk)**2, axis=1))
    bad = np.flatnonzero(be > ge * 0.01)
    print(f"\n256-sample blocks whose residual exceeds 1% of signal: {len(bad)} of {nblk}")
    if len(bad):
        print(f"first 24 bad block indices: {bad[:24]}")
        if len(bad) > 1:
            bd = np.diff(bad)
            vals, counts = np.unique(bd, return_counts=True)
            order = np.argsort(-counts)[:6]
            print("gaps between bad blocks:", ", ".join(f"{vals[i]}x{counts[i]}" for i in order))


if __name__ == '__main__':
    main()
