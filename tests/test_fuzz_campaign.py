#!/usr/bin/env python3
"""Time-bounded, reproducible ISO/UDF/raw-CD mutation campaign.

Black-box parser fuzzing (not coverage-guided libFuzzer). No customer images,
network access, elevated privileges or disk writes outside a temporary folder.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time
from test_images import iso_image, udf_image, raw_cd_image, B


def mutate(original: bytes, regions: list[tuple[int, int]], rng: random.Random) -> bytes:
    data = bytearray(original)
    start, end = rng.choice(regions)
    start, end = max(0, start), min(len(data), end)
    if start >= end:
        raise ValueError("invalid mutation range")
    strategy = rng.randrange(5)
    if strategy == 0:
        for _ in range(rng.randint(1, 32)):
            data[rng.randrange(start, end)] = rng.randrange(256)
    elif strategy == 1:
        at = rng.randrange(start, end)
        width = min(rng.choice((1, 2, 4, 8)), end-at)
        data[at:at+width] = rng.choice((0, 255)).to_bytes(1, "little") * width
    elif strategy == 2:
        at = rng.randrange(start, end)
        data[at] ^= 1 << rng.randrange(8)
    elif strategy == 3:
        at = rng.randrange(start, end)
        width = min(rng.randint(1, 128), end-at)
        data[at:at+width] = bytes(width)
    else:
        at = rng.randrange(start, end)
        width = min(rng.choice((2, 4, 8)), end-at)
        if width:
            data[at:at+width] = rng.getrandbits(width*8).to_bytes(width, "little")
    return bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("parser", type=Path)
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--seed", type=lambda x: int(x, 0), default=0x15A0D)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--start-case", type=int, default=0,
                    help="Skip parser invocations before this deterministic case")
    ap.add_argument("--max-cases", type=int, default=0,
                    help="Stop after N cases; 0 means use time limit only")
    args = ap.parse_args()
    if args.seconds < 1 or args.seconds > 3600 or args.timeout <= 0 or args.max_cases < 0 or args.start_case < 0:
        ap.error("invalid campaign bounds")
    rng = random.Random(args.seed)
    seeds = [
        ("iso", bytes(iso_image()), [(16*B, 19*B), (20*B, 22*B)]),
        ("udf", bytes(udf_image()), [(256*B, 263*B), (300*B, 304*B)]),
        ("rawcd", bytes(raw_cd_image(iso_image())),
         [(16*2352+16, 19*2352), (20*2352+16, 22*2352)]),
    ]
    deadline = time.monotonic() + args.seconds
    failure_dir = Path(os.environ.get("ISO_WCX_FUZZ_ARTIFACT_DIR", "."))
    failure_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    # RNG state must advance for every earlier case to reproduce the same
    # mutation. Do not invoke the parser for skipped cases.
    while count < args.start_case:
        label, original, regions = seeds[count % len(seeds)]
        mutate(original, regions, rng)
        count += 1
    processed = 0
    with tempfile.TemporaryDirectory(prefix="iso-wcx-fuzz-") as tmp:
        case = Path(tmp) / "input.iso"
        while time.monotonic() < deadline and (not args.max_cases or processed < args.max_cases):
            label, original, regions = seeds[count % len(seeds)]
            payload = mutate(original, regions, rng)
            case.write_bytes(payload)
            try:
                result = subprocess.run([str(args.parser.resolve()), str(case)],
                                        capture_output=True, timeout=args.timeout)
            except subprocess.TimeoutExpired as error:
                print(f"FAIL timeout seed={args.seed} case={count} format={label}", file=sys.stderr)
                (failure_dir / "fuzz-crash.iso").write_bytes(payload)
                (failure_dir / "fuzz-crash.txt").write_text(
                    f"seed={args.seed} case={count} format={label} reason=timeout\n")
                raise AssertionError("parser timed out; saved fuzz-crash.iso") from error
            sanitizer_failure = any(marker in result.stderr for marker in (
                b"ERROR: AddressSanitizer", b"runtime error:",
                b"UndefinedBehaviorSanitizer", b"LeakSanitizer"))
            if sanitizer_failure or result.returncode not in (0, 1, 2):
                print(f"FAIL crash seed={args.seed} case={count} format={label} "
                      f"exit={result.returncode} stderr={result.stderr[-300:]!r}",
                      file=sys.stderr)
                (failure_dir / "fuzz-crash.iso").write_bytes(payload)
                (failure_dir / "fuzz-crash.txt").write_text(
                    f"seed={args.seed} case={count} format={label} "
                    f"exit={result.returncode}\n"
                    + result.stderr.decode("utf-8", errors="replace")[-24000:])
                return 1
            count += 1
            processed += 1
    print(f"PASS black-box mutation campaign cases={count} seed={args.seed} seconds={args.seconds}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
