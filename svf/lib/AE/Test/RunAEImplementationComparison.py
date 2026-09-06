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
        or style not in {"injecting", "plain"}
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
    return {
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
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")

    failures = []
    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        output_file.flush()
        for input_index, (input_label, input_path) in enumerate(options.input):
            for phase, repetitions in (("hash", 1), ("performance", options.repetitions)):
                for repetition in range(1, repetitions + 1):
                    offset = (input_index + repetition - 1) % len(options.candidate)
                    order = options.candidate[offset:] + options.candidate[:offset]
                    for selected in order:
                        result = run_once(
                            options, selected, input_path, phase, repetition
                        )
                        result["input"] = input_label
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
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    main()
