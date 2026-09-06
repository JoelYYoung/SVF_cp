#!/usr/bin/env python3

"""Compare native AE implementations and their canonical result hashes."""

import argparse
import csv
import os
import pathlib
import platform
import re
import resource
import signal
import subprocess
import tempfile
import time


FIELDS = (
    "host",
    "input",
    "candidate",
    "phase",
    "repetition",
    "status",
    "seconds",
    "peak_rss_bytes",
    "analyzed_nodes",
    "result_hash",
    "result_records",
    "return_code",
    "diagnostic",
)

DIFFERENCE_FIELDS = (
    "host",
    "input",
    "reference",
    "candidate",
    "hash_equal",
    "reference_only_records",
    "candidate_only_records",
    "first_reference_only_record",
    "first_candidate_only_record",
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
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            "candidate must be LABEL=PERF_RUNNER,HASH_RUNNER,EXTAPI,HASH_STYLE"
        )
    label, raw_fields = value.split("=", 1)
    fields = raw_fields.split(",")
    if len(fields) != 4:
        raise argparse.ArgumentTypeError(
            "candidate must be LABEL=PERF_RUNNER,HASH_RUNNER,EXTAPI,HASH_STYLE"
        )
    performance_runner = pathlib.Path(fields[0])
    hash_runner = pathlib.Path(fields[1])
    extapi = pathlib.Path(fields[2])
    style = fields[3]
    if (
        not label
        or not performance_runner.is_file()
        or not hash_runner.is_file()
        or not extapi.is_file()
        or style not in {"injecting", "plain", "none"}
    ):
        raise argparse.ArgumentTypeError(f"invalid candidate: {value}")
    return label, performance_runner, hash_runner, extapi, style


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


def run_once(options, selected, input_path, phase, repetition):
    label, performance_runner, hash_runner, extapi, hash_style = selected
    runner = hash_runner if phase == "hash" else performance_runner
    environment = os.environ.copy()
    if phase == "hash":
        environment["SVF_AE_RESULT_HASH"] = "1"
        environment["SVF_AE_SEMANTIC_CHECKSUM"] = "1"
        if options.record_differences_output:
            environment["SVF_AE_RESULT_RECORDS"] = "1"
    command = [
        "/usr/bin/time",
        "-v",
        "-o",
        "TIME_OUTPUT",
        str(runner),
        f"-extapi={extapi}",
        "-ae-sparsity=semi-sparse",
        "-stat=false",
    ]
    if phase == "hash" and hash_style == "plain":
        command.extend(
            [
                "-model-consts=true",
                "-model-arrays=true",
                "-pre-field-sensitive=false",
            ]
        )
    command.append(str(input_path))

    with tempfile.NamedTemporaryFile() as time_output:
        command[3] = time_output.name
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
    result = re.search(
        r"AE_RESULT_HASH fnv1a64=([0-9a-f]+) records=(\d+)", output
    )
    lines = output.rstrip().splitlines()
    result_row = {
        "host": platform.node(),
        "candidate": label,
        "phase": phase,
        "repetition": repetition,
        "status": status,
        "seconds": f"{elapsed:.6f}",
        "peak_rss_bytes": rss if rss is not None else "",
        "analyzed_nodes": nodes.group(1) if nodes else "",
        "result_hash": result.group(1) if result else "",
        "result_records": result.group(2) if result else "",
        "return_code": process.returncode,
        "diagnostic": lines[-1][-500:] if lines else "",
    }
    result_records = re.findall(r"^AE_RESULT_RECORD (.*)$", output, re.MULTILINE)
    return result_row, result_records


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
    parser.add_argument("--require-all-pass", action="store_true")
    parser.add_argument("--record-differences-output")
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")
    if all(selected[4] == "none" for selected in options.candidate):
        parser.error("at least one candidate must provide a hash runner")

    failures = []
    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    differences_file = None
    differences_writer = None
    if options.record_differences_output:
        differences_path = pathlib.Path(options.record_differences_output)
        differences_path.parent.mkdir(parents=True, exist_ok=True)
        differences_file = differences_path.open("w", newline="")
        differences_writer = csv.DictWriter(
            differences_file, fieldnames=DIFFERENCE_FIELDS
        )
        differences_writer.writeheader()
        differences_file.flush()
    output_file = output_path.open("w", newline="")
    try:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        output_file.flush()
        for input_index, (input_label, input_path) in enumerate(options.input):
            hash_rows = {}
            hash_records = {}
            for phase, repetitions in (("hash", 1), ("performance", options.repetitions)):
                eligible = [
                    selected
                    for selected in options.candidate
                    if phase != "hash" or selected[4] != "none"
                ]
                for repetition in range(1, repetitions + 1):
                    offset = (input_index + repetition - 1) % len(eligible)
                    order = eligible[offset:] + eligible[:offset]
                    for selected in order:
                        result, records = run_once(
                            options, selected, input_path, phase, repetition
                        )
                        result["input"] = input_label
                        if phase == "hash":
                            hash_rows[selected[0]] = result
                            hash_records[selected[0]] = set(records)
                        writer.writerow(result)
                        output_file.flush()
                        print(
                            f"{input_label:18s} {selected[0]:16s} "
                            f"{phase:11s} {result['status']:7s} "
                            f"{result['seconds']}s rss={result['peak_rss_bytes']} "
                            f"result={result['result_hash']} "
                            f"records={result['result_records']}",
                            flush=True,
                        )
                        if options.require_all_pass and result["status"] != "pass":
                            failures.append(
                                f"{input_label}/{selected[0]}/{phase}#"
                                f"{repetition}={result['status']}"
                            )
                        if (
                            options.require_all_pass
                            and phase == "hash"
                            and not result["result_hash"]
                        ):
                            failures.append(
                                f"{input_label}/{selected[0]} missing result hash"
                            )
                if phase == "hash" and differences_writer:
                    reference = eligible[0][0]
                    reference_records = hash_records[reference]
                    for selected in eligible:
                        label = selected[0]
                        candidate_records = hash_records[label]
                        reference_only = sorted(
                            reference_records - candidate_records
                        )
                        candidate_only = sorted(
                            candidate_records - reference_records
                        )
                        differences_writer.writerow(
                            {
                                "host": platform.node(),
                                "input": input_label,
                                "reference": reference,
                                "candidate": label,
                                "hash_equal": str(
                                    hash_rows[reference]["result_hash"]
                                    == hash_rows[label]["result_hash"]
                                ).lower(),
                                "reference_only_records": len(reference_only),
                                "candidate_only_records": len(candidate_only),
                                "first_reference_only_record": (
                                    reference_only[0] if reference_only else ""
                                ),
                                "first_candidate_only_record": (
                                    candidate_only[0] if candidate_only else ""
                                ),
                            }
                        )
                    differences_file.flush()
    finally:
        output_file.close()
        if differences_file:
            differences_file.close()
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    main()
