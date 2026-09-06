#!/usr/bin/env python3

"""Check canonical Box/Address results across native sparse AE schedules."""

import argparse
import os
import pathlib
import re
import subprocess


RESULT = re.compile(r"AE_RESULT_HASH fnv1a64=([0-9a-f]+) records=(\d+)")


def run(runner, extapi, fixture, sparsity):
    environment = os.environ.copy()
    environment["SVF_AE_SEMANTIC_CHECKSUM"] = "1"
    command = [
        str(runner),
        f"-extapi={extapi}",
        f"-ae-sparsity={sparsity}",
        "-widen-delay=1",
        "-model-consts=true",
        "-model-arrays=true",
        "-pre-field-sensitive=false",
        "-stat=false",
        str(fixture),
    ]
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=environment,
        check=False,
    )
    match = RESULT.search(completed.stdout)
    if completed.returncode != 0 or not match:
        raise RuntimeError(
            f"{fixture.name}/{sparsity} failed with "
            f"return code {completed.returncode}:\n{completed.stdout}"
        )
    return match.group(1), int(match.group(2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=pathlib.Path, required=True)
    parser.add_argument("--extapi", type=pathlib.Path, required=True)
    parser.add_argument(
        "--fixture", action="append", type=pathlib.Path, required=True
    )
    options = parser.parse_args()
    for path in (options.runner, options.extapi, *options.fixture):
        if not path.is_file():
            parser.error(f"file does not exist: {path}")

    for fixture in options.fixture:
        semi_sparse = run(
            options.runner, options.extapi, fixture, "semi-sparse"
        )
        full_sparse = run(options.runner, options.extapi, fixture, "sparse")
        if semi_sparse != full_sparse:
            raise RuntimeError(
                f"{fixture.name}: semi-sparse result {semi_sparse} "
                f"!= sparse result {full_sparse}"
            )
        print(
            f"AE_SPARSITY_RESULT fixture={fixture.name} "
            f"fnv1a64={semi_sparse[0]} records={semi_sparse[1]}"
        )
    print(f"AE_SPARSITY_EQUIVALENCE PASS fixtures={len(options.fixture)}")


if __name__ == "__main__":
    main()
