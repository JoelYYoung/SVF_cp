#!/usr/bin/env python3

"""Compare native AE implementations and their canonical result hashes."""

import argparse
import csv
import fractions
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
    "ae_sparsity",
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
    "comparison_status",
    "incomparable_reason",
    "reference_status",
    "candidate_status",
    "hash_equal",
    "equal_query_answers",
    "reference_only_queries",
    "candidate_only_queries",
    "different_query_answers",
    "reachability_differences",
    "numeric_differences",
    "address_differences",
    "freed_differences",
    "reference_top_candidate_non_top",
    "reference_non_top_candidate_top",
    "candidate_superset_answers",
    "candidate_subset_answers",
    "incomparable_answers",
    "unclassified_answers",
    "first_different_query",
    "first_reference_answer",
    "first_candidate_answer",
    "reference_only_records",
    "candidate_only_records",
    "first_reference_only_record",
    "first_candidate_only_record",
)

QUERY_DIFFERENCE_FIELDS = (
    "host",
    "input",
    "reference",
    "candidate",
    "query",
    "difference",
    "reference_answer",
    "candidate_answer",
    "lattice_relation",
)


def query_projection(records):
    """Index canonical records by their semantic query, excluding the answer."""
    projection = {}
    for record in records:
        fields = record.split("|", 4)
        if fields[0] == "R" and len(fields) == 3:
            key, answer = "|".join(fields[:2]), fields[2]
        elif fields[0] == "F" and len(fields) == 4:
            key, answer = "|".join(fields[:3]), fields[3]
        elif fields[0] in {"V", "M"} and len(fields) == 5:
            key, answer = "|".join(fields[:4]), fields[4]
        else:
            raise RuntimeError(f"malformed semantic query record: {record}")
        previous = projection.setdefault(key, answer)
        if previous != answer:
            raise RuntimeError(
                f"semantic query has multiple answers: {key}: "
                f"{previous} and {answer}"
            )
    return projection


def difference_kind(key):
    fields = key.split("|")
    if fields[0] in {"R", "F"}:
        return fields[0]
    return fields[3]


def parse_interval(answer):
    if answer in {"bottom", "top"}:
        return answer
    match = re.fullmatch(r"([\[(])(.+), (.+)([\])])", answer)
    if not match:
        raise ValueError(f"malformed interval: {answer}")

    def endpoint(value):
        if value in {"-inf", "-oo", "+inf", "inf", "+oo", "oo"}:
            return value[0] if value[0] in {"-", "+"} else "+"
        return fractions.Fraction(value)

    return (
        endpoint(match.group(2)),
        endpoint(match.group(3)),
        match.group(1) == "(",
        match.group(4) == ")",
    )


def parse_address_set(answer):
    if answer in {"bottom", "top"}:
        return answer
    if not answer.startswith("{") or not answer.endswith("}"):
        raise ValueError(f"malformed address set: {answer}")
    contents = answer[1:-1]
    locations = set()
    position = 0
    while position < len(contents):
        colon = contents.find(":", position)
        if colon < 0:
            raise ValueError(f"malformed address element: {answer}")
        length = int(contents[position:colon])
        begin = colon + 1
        end = begin + length
        if end > len(contents):
            raise ValueError(f"truncated address element: {answer}")
        locations.add(contents[begin:end])
        position = end
        if position < len(contents):
            if contents[position] != ",":
                raise ValueError(f"malformed address separator: {answer}")
            position += 1
    return locations


def abstract_subset(left, right, kind):
    """Whether one canonical numeric/address answer is below another."""
    left = parse_interval(left) if kind == "N" else parse_address_set(left)
    right = parse_interval(right) if kind == "N" else parse_address_set(right)
    if left == "bottom" or right == "top":
        return True
    if left == "top" or right == "bottom":
        return left == right
    if kind == "A":
        return left <= right

    left_lower, left_upper, left_lower_strict, left_upper_strict = left
    right_lower, right_upper, right_lower_strict, right_upper_strict = right
    lower_contained = right_lower == "-" or (
        left_lower != "-"
        and (
            left_lower > right_lower
            or (
                left_lower == right_lower
                and (not right_lower_strict or left_lower_strict)
            )
        )
    )
    upper_contained = right_upper == "+" or (
        left_upper != "+"
        and (
            left_upper < right_upper
            or (
                left_upper == right_upper
                and (not right_upper_strict or left_upper_strict)
            )
        )
    )
    return lower_contained and upper_contained


def lattice_relation(key, reference_answer, candidate_answer):
    kind = difference_kind(key)
    if kind not in {"N", "A"}:
        return "not-applicable"
    try:
        reference_subset = abstract_subset(
            reference_answer, candidate_answer, kind
        )
        candidate_subset = abstract_subset(
            candidate_answer, reference_answer, kind
        )
    except (ValueError, ZeroDivisionError):
        return "unclassified"
    if reference_subset and candidate_subset:
        return "equal"
    if reference_subset:
        return "candidate-superset"
    if candidate_subset:
        return "candidate-subset"
    return "incomparable"


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
            "candidate must be "
            "LABEL=PERF_RUNNER,HASH_RUNNER,EXTAPI,HASH_STYLE[,AE_SPARSITY]"
        )
    label, raw_fields = value.split("=", 1)
    fields = raw_fields.split(",")
    if len(fields) not in {4, 5}:
        raise argparse.ArgumentTypeError(
            "candidate must be "
            "LABEL=PERF_RUNNER,HASH_RUNNER,EXTAPI,HASH_STYLE[,AE_SPARSITY]"
        )
    performance_runner, hash_runner, extapi = map(pathlib.Path, fields[:3])
    style = fields[3]
    sparsity = fields[4] if len(fields) == 5 else "semi-sparse"
    if (
        not label
        or not performance_runner.is_file()
        or not hash_runner.is_file()
        or not extapi.is_file()
        or style not in {"injecting", "plain", "none"}
        or sparsity not in {"dense", "semi-sparse", "sparse"}
    ):
        raise argparse.ArgumentTypeError(f"invalid candidate: {value}")
    return label, performance_runner, hash_runner, extapi, style, sparsity


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
    (
        label,
        performance_runner,
        hash_runner,
        extapi,
        hash_style,
        sparsity,
    ) = selected
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
        f"-ae-sparsity={sparsity}",
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
        "ae_sparsity": sparsity,
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
    parser.add_argument("--hash-repetitions", type=int, choices=(0, 1), default=1)
    parser.add_argument("--timeout", type=float, default=86400.0)
    parser.add_argument(
        "--memory-bytes", type=int, default=512 * 1024 * 1024 * 1024
    )
    parser.add_argument("--require-all-pass", action="store_true")
    parser.add_argument("--record-differences-output")
    parser.add_argument("--query-differences-output")
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions < 0 or options.timeout <= 0:
        parser.error("repetitions must be non-negative and timeout positive")
    if options.repetitions == 0 and options.hash_repetitions == 0:
        parser.error("at least one hash or performance repetition is required")
    if options.hash_repetitions and all(
        selected[4] == "none" for selected in options.candidate
    ):
        parser.error("at least one candidate must provide a hash runner")
    if options.record_differences_output and not options.hash_repetitions:
        parser.error("record differences require a hash repetition")
    if (
        options.query_differences_output
        and not options.record_differences_output
    ):
        parser.error("query differences require record differences")

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
    query_differences_file = None
    query_differences_writer = None
    if options.query_differences_output:
        query_differences_path = pathlib.Path(options.query_differences_output)
        query_differences_path.parent.mkdir(parents=True, exist_ok=True)
        query_differences_file = query_differences_path.open("w", newline="")
        query_differences_writer = csv.DictWriter(
            query_differences_file, fieldnames=QUERY_DIFFERENCE_FIELDS
        )
        query_differences_writer.writeheader()
        query_differences_file.flush()
    output_file = output_path.open("w", newline="")
    try:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        output_file.flush()
        for input_index, (input_label, input_path) in enumerate(options.input):
            hash_rows = {}
            hash_records = {}
            for phase, repetitions in (
                ("hash", options.hash_repetitions),
                ("performance", options.repetitions),
            ):
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
                    reference_row = hash_rows[reference]
                    for selected in eligible:
                        label = selected[0]
                        candidate_row = hash_rows[label]
                        reasons = []
                        for role, row in (
                            ("reference", reference_row),
                            ("candidate", candidate_row),
                        ):
                            if row["status"] != "pass":
                                reasons.append(f"{role}-{row['status']}")
                            elif not row["result_hash"]:
                                reasons.append(f"{role}-missing-hash")
                        comparable = not reasons
                        if comparable:
                            reference_records = hash_records[reference]
                            candidate_records = hash_records[label]
                            reference_queries = query_projection(
                                reference_records
                            )
                            candidate_queries = query_projection(
                                candidate_records
                            )
                            reference_keys = set(reference_queries)
                            candidate_keys = set(candidate_queries)
                            common_keys = reference_keys & candidate_keys
                            different_queries = sorted(
                                key
                                for key in common_keys
                                if reference_queries[key]
                                != candidate_queries[key]
                            )
                            equal_queries = len(common_keys) - len(
                                different_queries
                            )
                            kind_counts = {
                                kind: sum(
                                    difference_kind(key) == kind
                                    for key in different_queries
                                )
                                for kind in ("R", "N", "A", "F")
                            }
                            reference_top_candidate_non_top = sum(
                                reference_queries[key] == "top"
                                and candidate_queries[key] != "top"
                                for key in different_queries
                            )
                            reference_non_top_candidate_top = sum(
                                reference_queries[key] != "top"
                                and candidate_queries[key] == "top"
                                for key in different_queries
                            )
                            relations = {
                                key: lattice_relation(
                                    key,
                                    reference_queries[key],
                                    candidate_queries[key],
                                )
                                for key in different_queries
                            }
                            relation_counts = {
                                relation: sum(
                                    value == relation
                                    for value in relations.values()
                                )
                                for relation in (
                                    "candidate-superset",
                                    "candidate-subset",
                                    "incomparable",
                                    "unclassified",
                                )
                            }
                            reference_only = sorted(
                                set(reference_records) - set(candidate_records)
                            )
                            candidate_only = sorted(
                                set(candidate_records) - set(reference_records)
                            )
                        else:
                            reference_keys = set()
                            candidate_keys = set()
                            different_queries = []
                            equal_queries = 0
                            kind_counts = {
                                kind: 0 for kind in ("R", "N", "A", "F")
                            }
                            reference_top_candidate_non_top = 0
                            reference_non_top_candidate_top = 0
                            relations = {}
                            relation_counts = {
                                relation: 0
                                for relation in (
                                    "candidate-superset",
                                    "candidate-subset",
                                    "incomparable",
                                    "unclassified",
                                )
                            }
                            reference_only = []
                            candidate_only = []
                        differences_writer.writerow(
                            {
                                "host": platform.node(),
                                "input": input_label,
                                "reference": reference,
                                "candidate": label,
                                "comparison_status": (
                                    "comparable" if comparable else "incomparable"
                                ),
                                "incomparable_reason": ";".join(reasons),
                                "reference_status": reference_row["status"],
                                "candidate_status": candidate_row["status"],
                                "hash_equal": (
                                    str(
                                        reference_row["result_hash"]
                                        == candidate_row["result_hash"]
                                    ).lower()
                                    if comparable
                                    else ""
                                ),
                                "equal_query_answers": (
                                    equal_queries if comparable else ""
                                ),
                                "reference_only_queries": (
                                    len(reference_keys - candidate_keys)
                                    if comparable
                                    else ""
                                ),
                                "candidate_only_queries": (
                                    len(candidate_keys - reference_keys)
                                    if comparable
                                    else ""
                                ),
                                "different_query_answers": (
                                    len(different_queries)
                                    if comparable
                                    else ""
                                ),
                                "reachability_differences": (
                                    kind_counts["R"] if comparable else ""
                                ),
                                "numeric_differences": (
                                    kind_counts["N"] if comparable else ""
                                ),
                                "address_differences": (
                                    kind_counts["A"] if comparable else ""
                                ),
                                "freed_differences": (
                                    kind_counts["F"] if comparable else ""
                                ),
                                "reference_top_candidate_non_top": (
                                    reference_top_candidate_non_top
                                    if comparable
                                    else ""
                                ),
                                "reference_non_top_candidate_top": (
                                    reference_non_top_candidate_top
                                    if comparable
                                    else ""
                                ),
                                "candidate_superset_answers": (
                                    relation_counts["candidate-superset"]
                                    if comparable
                                    else ""
                                ),
                                "candidate_subset_answers": (
                                    relation_counts["candidate-subset"]
                                    if comparable
                                    else ""
                                ),
                                "incomparable_answers": (
                                    relation_counts["incomparable"]
                                    if comparable
                                    else ""
                                ),
                                "unclassified_answers": (
                                    relation_counts["unclassified"]
                                    if comparable
                                    else ""
                                ),
                                "first_different_query": (
                                    different_queries[0]
                                    if different_queries
                                    else ""
                                ),
                                "first_reference_answer": (
                                    reference_queries[different_queries[0]]
                                    if different_queries
                                    else ""
                                ),
                                "first_candidate_answer": (
                                    candidate_queries[different_queries[0]]
                                    if different_queries
                                    else ""
                                ),
                                "reference_only_records": (
                                    len(reference_only) if comparable else ""
                                ),
                                "candidate_only_records": (
                                    len(candidate_only) if comparable else ""
                                ),
                                "first_reference_only_record": (
                                    reference_only[0] if reference_only else ""
                                ),
                                "first_candidate_only_record": (
                                    candidate_only[0] if candidate_only else ""
                                ),
                            }
                        )
                        if comparable and query_differences_writer:
                            query_rows = []
                            for key in sorted(reference_keys - candidate_keys):
                                query_rows.append(
                                    (
                                        key,
                                        "reference-only",
                                        reference_queries[key],
                                        "",
                                    )
                                )
                            for key in sorted(candidate_keys - reference_keys):
                                query_rows.append(
                                    (
                                        key,
                                        "candidate-only",
                                        "",
                                        candidate_queries[key],
                                    )
                                )
                            for key in different_queries:
                                query_rows.append(
                                    (
                                        key,
                                        "answer-different",
                                        reference_queries[key],
                                        candidate_queries[key],
                                    )
                                )
                            for (
                                key,
                                difference,
                                reference_answer,
                                candidate_answer,
                            ) in query_rows:
                                query_differences_writer.writerow(
                                    {
                                        "host": platform.node(),
                                        "input": input_label,
                                        "reference": reference,
                                        "candidate": label,
                                        "query": key,
                                        "difference": difference,
                                        "reference_answer": reference_answer,
                                        "candidate_answer": candidate_answer,
                                        "lattice_relation": (
                                            relations.get(
                                                key, "not-applicable"
                                            )
                                            if difference == "answer-different"
                                            else "not-applicable"
                                        ),
                                    }
                                )
                    differences_file.flush()
                    if query_differences_file:
                        query_differences_file.flush()
    finally:
        output_file.close()
        if differences_file:
            differences_file.close()
        if query_differences_file:
            query_differences_file.close()
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    main()
