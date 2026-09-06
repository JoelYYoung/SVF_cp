#!/usr/bin/env python3

"""Summarize equal-program time/RSS ratios from storage comparison CSVs."""

import argparse
import csv
import math
import pathlib
import statistics


def geometric_mean(values):
    return math.exp(statistics.fmean(math.log(value) for value in values))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--expected-repetitions", type=int)
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.expected_repetitions is not None and options.expected_repetitions <= 0:
        parser.error("--expected-repetitions must be positive")

    rows = []
    for raw_path in options.input:
        with pathlib.Path(raw_path).open(newline="") as input_file:
            rows.extend(csv.DictReader(input_file))
    passed = [row for row in rows if row["status"] == "pass"]
    candidates = sorted({row["candidate"] for row in passed})
    if options.baseline not in candidates:
        raise RuntimeError("baseline has no passing samples")

    grouped = {}
    for row in passed:
        grouped.setdefault((row["input"], row["candidate"]), []).append(row)
    programs = sorted({program for program, _ in grouped})
    summary = []
    for program in programs:
        if any((program, candidate) not in grouped for candidate in candidates):
            continue
        signatures = {
            (row["analyzed_nodes"], row["semantic_shape_checksum"])
            for candidate in candidates
            for row in grouped[(program, candidate)]
            if row.get("semantic_shape_checksum")
        }
        if signatures and len(signatures) != 1:
            raise RuntimeError(f"semantic mismatch for {program}: {signatures}")
        medians = {}
        for candidate in candidates:
            samples = grouped[(program, candidate)]
            if (options.expected_repetitions is not None and
                    len(samples) != options.expected_repetitions):
                raise RuntimeError(
                    f"{program}/{candidate} has {len(samples)} samples; "
                    f"expected {options.expected_repetitions}"
                )
            times = [float(row["seconds"]) for row in samples]
            rss = [
                int(row["peak_rss_bytes"])
                for row in samples
                if row["peak_rss_bytes"]
            ]
            medians[candidate] = (
                statistics.median(times),
                statistics.median(rss) if rss else None,
            )
        baseline_time, baseline_rss = medians[options.baseline]
        for candidate in candidates:
            candidate_time, candidate_rss = medians[candidate]
            summary.append(
                {
                    "input": program,
                    "candidate": candidate,
                    "median_seconds": f"{candidate_time:.6f}",
                    "median_peak_rss_bytes": (
                        int(candidate_rss) if candidate_rss is not None else ""
                    ),
                    "time_ratio": f"{candidate_time / baseline_time:.9f}",
                    "rss_ratio": (
                        f"{candidate_rss / baseline_rss:.9f}"
                        if candidate_rss is not None and baseline_rss
                        else ""
                    ),
                }
            )

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "input",
        "candidate",
        "median_seconds",
        "median_peak_rss_bytes",
        "time_ratio",
        "rss_ratio",
    )
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary)

    for candidate in candidates:
        candidate_rows = [
            row for row in summary if row["candidate"] == candidate
        ]
        time_ratios = [float(row["time_ratio"]) for row in candidate_rows]
        rss_ratios = [
            float(row["rss_ratio"])
            for row in candidate_rows
            if row["rss_ratio"]
        ]
        print(
            f"{candidate}: programs={len(candidate_rows)} "
            f"time_geomean={geometric_mean(time_ratios):.6f} "
            f"rss_geomean={geometric_mean(rss_ratios):.6f}"
        )


if __name__ == "__main__":
    main()
