#!/usr/bin/env python3

"""Measure AddressDomain storage candidates on identical AE workloads."""

import argparse
import csv
import os
import pathlib
import platform
import re
import resource
import signal
import statistics
import subprocess
import tempfile
import time


FIELDS = (
    "host",
    "input",
    "candidate",
    "repetition",
    "status",
    "seconds",
    "peak_rss_bytes",
    "analyzed_nodes",
    "semantic_checksum",
    "return_code",
    "diagnostic",
)


def labeled_path(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = pathlib.Path(raw_path)
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid labeled path: {value}")
    return label, path


def candidate(value):
    if "=" not in value or "," not in value:
        raise argparse.ArgumentTypeError(
            "candidate must be LABEL=RUNNER,EXTAPI"
        )
    label, paths = value.split("=", 1)
    runner, extapi = (pathlib.Path(path) for path in paths.split(",", 1))
    if not label or not runner.is_file() or not extapi.is_file():
        raise argparse.ArgumentTypeError(f"invalid candidate: {value}")
    return label, runner, extapi


def peak_rss(text):
    match = re.search(
        r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$",
        text,
        re.MULTILINE,
    )
    return int(match.group(1)) * 1024 if match else None


def limit_address_space(bytes_limit):
    def apply_limit():
        resource.setrlimit(resource.RLIMIT_AS, (bytes_limit, bytes_limit))

    return apply_limit


def run_once(options, selected, input_path):
    label, runner, extapi = selected
    environment = os.environ.copy()
    if options.validate:
        environment["SVF_AE_SEMANTIC_CHECKSUM"] = "1"
        environment["SVF_AE_STORAGE_OBSERVATION"] = "1"
    command = [
        "/usr/bin/time",
        "-v",
        "-o",
        "TIME_OUTPUT",
        str(runner),
        f"-extapi={extapi}",
        "-ae-sparsity=semi-sparse",
        "-model-consts=true",
        "-model-arrays=true",
        "-pre-field-sensitive=false",
        "-stat=false",
        str(input_path),
    ]
    with tempfile.NamedTemporaryFile() as time_output:
        command[4] = time_output.name
        started = time.perf_counter()
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            env=environment,
            preexec_fn=limit_address_space(options.memory_bytes),
        )
        try:
            output, _ = process.communicate(timeout=options.timeout)
            status = "pass" if process.returncode == 0 else "fail"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            status = "timeout"
        elapsed = time.perf_counter() - started
        time_output.seek(0)
        rss = peak_rss(time_output.read().decode(errors="replace"))

    nodes = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    checksum = re.search(r"AE_SEMANTIC_CHECKSUM fnv1a64=([0-9a-f]+)", output)
    lines = output.rstrip().splitlines()
    return {
        "host": platform.node(),
        "candidate": label,
        "status": status,
        "seconds": f"{elapsed:.6f}",
        "peak_rss_bytes": rss if rss is not None else "",
        "analyzed_nodes": nodes.group(1) if nodes else "",
        "semantic_checksum": checksum.group(1) if checksum else "",
        "return_code": process.returncode,
        "diagnostic": lines[-1][-500:] if lines else "",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--candidate", action="append", type=candidate, required=True
    )
    parser.add_argument("--input", action="append", type=labeled_path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=86400.0)
    parser.add_argument(
        "--memory-bytes", type=int, default=512 * 1024 * 1024 * 1024
    )
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--require-all-pass", action="store_true")
    parser.add_argument("--require-semantic-match", action="store_true")
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")
    if options.require_semantic_match and not options.validate:
        parser.error("--require-semantic-match requires --validate")

    rows = []
    failures = []
    for input_index, (input_label, input_path) in enumerate(options.input):
        observations = {item[0]: [] for item in options.candidate}
        for repetition in range(1, options.repetitions + 1):
            offset = (input_index + repetition - 1) % len(options.candidate)
            order = options.candidate[offset:] + options.candidate[:offset]
            for selected in order:
                result = run_once(options, selected, input_path)
                result.update(input=input_label, repetition=repetition)
                rows.append(result)
                observations[selected[0]].append(result)
                print(
                    f"{input_label:18s} {selected[0]:20s} "
                    f"{result['status']:7s} {result['seconds']}s "
                    f"rss={result['peak_rss_bytes']} "
                    f"nodes={result['analyzed_nodes']} "
                    f"checksum={result['semantic_checksum']}",
                    flush=True,
                )

        if options.require_all_pass:
            for label, samples in observations.items():
                for sample in samples:
                    if sample["status"] != "pass":
                        failures.append(
                            f"{input_label}/{label}#{sample['repetition']}="
                            f"{sample['status']}"
                        )
        if options.require_semantic_match:
            signatures = {
                (sample["analyzed_nodes"], sample["semantic_checksum"])
                for samples in observations.values()
                for sample in samples
                if sample["status"] == "pass"
            }
            completed = sum(
                sample["status"] == "pass"
                for samples in observations.values()
                for sample in samples
            )
            expected = len(options.candidate) * options.repetitions
            if completed != expected or len(signatures) != 1 or ("", "") in signatures:
                failures.append(
                    f"{input_label}: semantic signatures={sorted(signatures)}"
                )

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    for label, _, _ in options.candidate:
        passed = [
            row for row in rows if row["candidate"] == label and row["status"] == "pass"
        ]
        if passed:
            times = [float(row["seconds"]) for row in passed]
            rss_values = [
                int(row["peak_rss_bytes"])
                for row in passed
                if row["peak_rss_bytes"] != ""
            ]
            print(
                f"SUMMARY {label} samples={len(passed)} "
                f"median_time={statistics.median(times):.6f}s "
                f"median_rss={statistics.median(rss_values) if rss_values else ''}",
                flush=True,
            )
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    main()
