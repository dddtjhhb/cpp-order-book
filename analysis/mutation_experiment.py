#!/usr/bin/env python3
"""Compare example-based unit tests with property fuzzing on temporary mutants."""

from __future__ import annotations

import csv
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Mutant:
    name: str
    old: str
    new: str


MUTANTS = [
    Mutant(
        "skip_total_update_on_execute",
        "level.total_quantity -= executed_quantity;",
        "/* mutant: skipped level total update */",
    ),
    Mutant(
        "skip_total_update_on_cancel",
        "level->second.total_quantity -= order.quantity;",
        "/* mutant: skipped level total update */",
    ),
    Mutant(
        "reset_priority_on_equal_quantity",
        "new_price == old.price && new_quantity <= old.quantity",
        "new_price == old.price && new_quantity < old.quantity",
    ),
    Mutant(
        "strict_buy_crossing",
        "incoming.price >= resting_price",
        "incoming.price > resting_price",
    ),
    Mutant(
        "strict_sell_crossing",
        "incoming.price <= resting_price",
        "incoming.price < resting_price",
    ),
    # Reintroduces the v0.6 bug: a marketable MODIFY rests without matching.
    Mutant(
        "modify_skips_matching",
        "return submit(replacement, timestamp_ns);",
        "add_resting(replacement);\n"
        "    return result(ResultCode::Accepted, \"mutant: rested without matching\", {}, new_quantity);",
    ),
    # Added in v0.8. The first three corrupt trade *records* while leaving book
    # state intact; they were chosen because the property fuzzer does not assert
    # trade prices, IDs, or timestamps, so they are biased toward the
    # differential harness. The fourth is a priority bug both should catch.
    Mutant(
        "trade_at_incoming_price",
        "resting_id, resting_price,",
        "resting_id, incoming.price,",
    ),
    Mutant(
        "trade_id_not_incremented",
        "Trade{next_trade_id_++,",
        "Trade{next_trade_id_,",
    ),
    Mutant(
        "trade_timestamp_dropped",
        "traded_quantity, timestamp_ns});",
        "traded_quantity, 0});",
    ),
    Mutant(
        "quantity_increase_keeps_priority",
        "new_price == old.price && new_quantity <= old.quantity",
        "new_price == old.price",
    ),
]

SEEDS = (1, 7, 42, 2026, 20260831)


def run(command: list[str], cwd: Path) -> bool:
    return subprocess.run(
        command, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False
    ).returncode == 0


def compile_test(source: Path, mutated_order_book: Path, output: Path, extra: list[Path]) -> bool:
    command = [
        "c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wpedantic",
        f"-I{ROOT / 'include'}", f"-I{ROOT / 'tests'}",
        str(mutated_order_book), *(str(path) for path in extra),
        str(source), "-o", str(output),
    ]
    return run(command, ROOT)


def all_seeds_pass(binary: Path, temp: Path, name: str, suffix: str) -> bool:
    """True if the mutant survives every seed; a kill must leave a reproducer."""
    for seed in SEEDS:
        failure_path = temp / f"{name}_{binary.name}_{seed}.{suffix}"
        if not run([str(binary), str(seed), "5000", str(failure_path)], temp):
            if not failure_path.exists():
                raise RuntimeError(f"{name} failed without a saved sequence")
            return False
    return True


def main() -> None:
    original = (ROOT / "src/order_book.cpp").read_text(encoding="utf-8")
    rows = []
    with tempfile.TemporaryDirectory(prefix="order-book-mutation-") as temp_name:
        temp = Path(temp_name)
        for mutant in MUTANTS:
            if original.count(mutant.old) != 1:
                raise RuntimeError(f"mutation target for {mutant.name} is not unique")
            mutated_source = temp / f"{mutant.name}.cpp"
            mutated_source.write_text(original.replace(mutant.old, mutant.new), encoding="utf-8")

            unit_survived = True
            for source_name in ("order_book_tests.cpp", "engine_contract_tests.cpp"):
                unit_binary = temp / f"{mutant.name}_{source_name}.unit"
                unit_compiled = compile_test(
                    ROOT / "tests" / source_name,
                    mutated_source,
                    unit_binary,
                    [ROOT / "src/csv_reader.cpp"],
                )
                if not unit_compiled:
                    raise RuntimeError(f"{mutant.name}: {source_name} did not compile")
                unit_survived = run([str(unit_binary)], temp) and unit_survived

            fuzz_binary = temp / f"{mutant.name}_fuzz"
            fuzz_compiled = compile_test(
                ROOT / "tests/order_book_property_fuzz.cpp", mutated_source, fuzz_binary, []
            )
            if not fuzz_compiled:
                raise RuntimeError(f"{mutant.name}: property fuzzer did not compile")
            fuzz_survived = fuzz_compiled and all_seeds_pass(fuzz_binary, temp, mutant.name, "txt")

            diff_binary = temp / f"{mutant.name}_diff"
            diff_compiled = compile_test(
                ROOT / "tests/order_book_differential.cpp",
                mutated_source,
                diff_binary,
                [ROOT / "src/csv_reader.cpp"],
            )
            if not diff_compiled:
                raise RuntimeError(f"{mutant.name}: differential fuzzer did not compile")
            diff_survived = diff_compiled and all_seeds_pass(diff_binary, temp, mutant.name, "csv")

            rows.append(
                {
                    "mutant": mutant.name,
                    "unit_tests": "SURVIVED" if unit_survived else "KILLED",
                    "property_fuzzer": "SURVIVED" if fuzz_survived else "KILLED",
                    "differential": "SURVIVED" if diff_survived else "KILLED",
                }
            )

    output = ROOT / "docs/testing/mutation_results.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(
            f"{row['mutant']}: unit={row['unit_tests']} fuzz={row['property_fuzzer']}"
            f" differential={row['differential']}"
        )
    print(f"results_file={output}")


if __name__ == "__main__":
    main()
