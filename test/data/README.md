# PubGrub Solver Test Files

This directory contains test files for the PubGrub solver implementation. Tests are written in a simple JSON format that allows us to define package dependency scenarios and verify that the solver produces the expected results.

## Test File Format

Test files should be named with a numeric prefix (e.g., `01-test-name.json`, `02-another-test.json`) to ensure they're processed by the test runner.

### Structure

```json
{
  "name": "Test Name",
  "description": "Optional description of what this test verifies",
  "packages": {
    "package_name": {
      "versions": ["1.0.0", "2.0.0"],
      "dependencies": {
        "1.0.0": {
          "dep_package": "^1.0.0"
        },
        "2.0.0": {}
      }
    }
  },
  "root_dependencies": {
    "package_name": "^1.0.0"
  },
  "expected": "success",
  "solution": {
    "package_name": "1.0.0"
  }
}
```

### Fields

- **name** (required): A human-readable name for the test
- **description** (optional): A description of what the test verifies
- **packages** (required): Object defining all packages available in the test universe
  - Each package has:
    - **versions**: Array of version strings (e.g., `["1.0.0", "2.0.0"]`)
    - **dependencies**: Object mapping version strings to their dependencies
      - Each version maps to an object where keys are package names and values are version ranges
- **root_dependencies** (required): Dependencies from the root package
  - Object where keys are package names and values are version ranges
- **expected** (required): Either `"success"` or `"conflict"`
  - `"success"` means the solver should find a valid solution
  - `"conflict"` means the solver should fail to find a solution
- **solution** (optional): For success cases, the expected package versions
  - Object where keys are package names and values are version strings

### Version Range Syntax

The test parser supports the following version range patterns:

- `"any"` - Any version
- `"^1.0.0"` - Caret range (until next major version)
- `">=1.0.0"` - Greater than or equal (approximated as until next major)
- `"1.0.0"` - Exact version match

## Running Tests

### Build and run the test suite

```bash
make test
```

This will run both the basic solver tests and all file-based tests.

### Run only the file-based tests

```bash
make pg_file_test
./bin/pg_file_test
```

### Run tests from a different directory

```bash
./bin/pg_file_test /path/to/test/files
```

## Test Examples

The following tests are included from the PubGrub documentation:

1. **01-no-conflicts.json**: Simple case with no conflicts
2. **02-avoiding-conflict-during-decision.json**: Decision making that avoids conflicts
3. **03-performing-conflict-resolution.json**: Full conflict resolution with backtracking
4. **04-linear-error-reporting.json**: Unsolvable scenario with linear derivation
5. **05-branching-error-reporting.json**: Complex unsolvable scenario with branching derivation

## Adding New Tests

To add a new test:

1. Create a new JSON file with a numeric prefix (e.g., `06-my-test.json`)
2. Follow the test file format described above
3. Run `make test` to verify your test works correctly

## Notes

- Test files must start with a two-digit prefix (01-09) to be recognized by the test runner
- Other JSON files in this directory (like `all-packages.json`, `test-no-upgrades.json`) are skipped
- The test runner will skip any JSON files that don't have a "name" field
