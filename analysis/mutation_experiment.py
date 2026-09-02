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
]


def run(command: list[str], cwd: Path) -> bool:
    return subprocess.run(
        command, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False
    ).returncode == 0


def compile_test(source: Path, mutated_order_book: Path, output: Path, extra: list[Path]) -> bool:
    command = [
        "c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wpedantic",
        f"-I{ROOT / 'include'}", str(mutated_order_book), *(str(path) for path in extra),
        str(source), "-o", str(output),
    ]
    return run(command, ROOT)


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

            unit_binary = temp / f"{mutant.name}_unit"
            unit_compiled = compile_test(
                ROOT / "tests/order_book_tests.cpp",
                mutated_source,
                unit_binary,
                [ROOT / "src/csv_reader.cpp"],
            )
            unit_survived = unit_compiled and run([str(unit_binary)], temp)

            fuzz_binary = temp / f"{mutant.name}_fuzz"
            fuzz_compiled = compile_test(
                ROOT / "tests/order_book_property_fuzz.cpp", mutated_source, fuzz_binary, []
            )
            fuzz_survived = fuzz_compiled
            if fuzz_compiled:
                for seed in (1, 7, 42, 2026, 20260831):
                    failure_path = temp / f"{mutant.name}_failure.txt"
                    if not run([str(fuzz_binary), str(seed), "5000", str(failure_path)], temp):
                        if not failure_path.exists():
                            raise RuntimeError(f"{mutant.name} failed without a saved sequence")
                        fuzz_survived = False
                        break

            rows.append(
                {
                    "mutant": mutant.name,
                    "unit_tests": "SURVIVED" if unit_survived else "KILLED",
                    "property_fuzzer": "SURVIVED" if fuzz_survived else "KILLED",
                }
            )

    output = ROOT / "docs/testing/mutation_results.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(f"{row['mutant']}: unit={row['unit_tests']} fuzz={row['property_fuzzer']}")
    print(f"results_file={output}")


if __name__ == "__main__":
    main()
