# Box versus legacy Semi-Sparse AE

Date: 2026-09-03

## Question and decision rule

This experiment compares the current Box-only Semi-Sparse AE with the original
Semi-Sparse implementation backed by `IntervalState` and its compatibility
abstract trace. It is a version-level comparison, not an isolated measurement
of the Box page directory.

A program enters the aggregate only when both implementations finish and
report the same number of analyzed ICFG nodes under the production input-model
options. At least ten admitted real programs and five fresh processes per cell
are required. A node-count match is a coverage guard, not a proof that every
abstract value is identical; checked-in semantic fixtures provide the
operation-level regression checks.

## Configurations

| Configuration | Revision | Semi-Sparse carrier |
|---|---|---|
| Legacy | `40d8de3f` | legacy `IntervalState` plus compatibility trace |
| Box | `562c5cbc` | `BoxAddressDomain`, global scalar carrier and ICFG memory carrier |

Both revisions were built separately with Homebrew Clang 21.1.8, C++17,
`Release`, assertions off and LTO off. Both use their own generated `extapi.bc`.
The timed executable is the production `ae` driver, which enables constant and
array modeling and disables pre-field sensitivity on both sides. The admission
runner receives those same options explicitly.

The host is an Apple M5 with 32 GiB RAM running macOS 26.6.2. Every sample is a
fresh process. Legacy/Box order alternates by program and repetition. Wall time
and maximum RSS come from `/usr/bin/time -l`; the timeout is 180 seconds. There
is no artificial memory cap. Each timed cell has five repetitions and the
table reports medians.

## Results

| Program | Nodes old/new | Legacy time | Box time | Time delta | Legacy RSS | Box RSS | RSS delta |
|---|---:|---:|---:|---:|---:|---:|---:|
| mp4subtitle | 999 / 999 | 0.1728 s | 0.0443 s | -74.38% | 107.20 MiB | 36.77 MiB | -65.70% |
| mp4file | 880 / 880 | 0.1361 s | 0.0501 s | -63.20% | 100.78 MiB | 40.58 MiB | -59.74% |
| mp4art | 1,328 / 1,328 | 0.7887 s | 0.1018 s | -87.09% | 304.45 MiB | 57.11 MiB | -81.24% |
| mp4track | 3,309 / 3,309 | 22.0214 s | 0.5052 s | -97.71% | 1,458.28 MiB | 86.47 MiB | -94.07% |
| curl_min | 7,938 / 7,938 | 2.2155 s | 0.1365 s | -93.84% | 3,351.77 MiB | 99.05 MiB | -97.04% |
| faad2 | 7,964 / 7,964 | 2.2612 s | 0.1374 s | -93.92% | 3,359.02 MiB | 99.86 MiB | -97.03% |
| hdf5 | 7,978 / 7,978 | 2.2382 s | 0.1394 s | -93.77% | 3,368.08 MiB | 99.89 MiB | -97.03% |
| espeak | 7,955 / 7,955 | 2.3568 s | 0.1403 s | -94.05% | 3,361.23 MiB | 99.97 MiB | -97.03% |
| dav1d | 7,991 / 7,991 | 2.6344 s | 0.1479 s | -94.39% | 3,381.67 MiB | 100.41 MiB | -97.03% |
| htslib | 7,954 / 7,954 | 2.3494 s | 0.1392 s | -94.08% | 3,360.50 MiB | 99.72 MiB | -97.03% |
| libyaml | 7,893 / 7,893 | 2.3088 s | 0.1413 s | -93.88% | 3,319.22 MiB | 98.27 MiB | -97.04% |
| dnsmasq | 7,940 / 7,940 | 2.3575 s | 0.1396 s | -94.08% | 3,352.86 MiB | 99.27 MiB | -97.04% |
| c-ares | 7,889 / 7,889 | 2.4399 s | 0.1446 s | -94.07% | 3,317.75 MiB | 98.41 MiB | -97.03% |

Across the 13 admitted programs:

| Aggregate | Time | Peak RSS |
|---|---:|---:|
| Equal-program geometric-mean delta | -92.41% | -94.68% |
| Wins | 13 / 13 | 13 / 13 |
| Sum of per-program median times | 44.2805 s -> 1.9675 s (-95.56%) | not additive |

`c-blosc2` is retained in the raw and summary data but excluded from these
aggregates: legacy analyzes 8,620 nodes and Box analyzes 8,627 under production
modeling. Its exploratory medians are 12.3077 s versus 1.7256 s and 4,139.84
MiB versus 127.94 MiB, but these are not a strict same-work comparison.

## Interpretation

The current implementation decisively improves this production workload. The
largest effect is not attributable to page COW alone. The legacy production
path maintains the compatibility `IntervalState` trace in addition to sparse
state, whereas the current path has one native Box-based state system with a
global scalar carrier and ICFG-attached memory carrier. Removing that duplicate
state and its copying/maintenance cost is part of the measured improvement.
Other changes between the two revisions also contribute, so this experiment
supports deployment of the current complete design but does not estimate a
standalone causal effect for any single optimization.

During admission, modeled floating constants exposed an existing conversion
bug: decimal text such as `0.000000` was passed to GMP's rational parser. The
Box implementation now converts finite floating values directly to exact GMP
rationals and rejects non-finite values. Debug and Release regression suites
both pass all 15 tests after the fix.

## Artifacts

- `RunBoxSemiSparseComparison.py`: reproducible interleaved harness.
- `AESemiSparseBoxVsLegacyAdmission-2026-09-03.csv`: one production-option
  admission run per implementation and program.
- `AESemiSparseBoxVsLegacyResults-2026-09-03.csv`: all 140 timed raw samples.
- `AESemiSparseBoxVsLegacySummary-2026-09-03.csv`: medians and admission status.
