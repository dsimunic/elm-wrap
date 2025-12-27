# Contributing to elm-wrap

Thank you for your interest in contributing to elm-wrap!

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Build the project: `make rebuild`
4. Run tests: `make check`

See [`doc/BUILDING.md`](doc/BUILDING.md) for detailed build instructions.

## Submitting Changes

### Pull Request Process

1. Create a feature branch from `main`
2. Make your changes
3. Ensure all tests pass: `make check`
4. Ensure the build succeeds with no warnings (the project uses `-Wall -Werror`)
5. Submit a pull request with a clear description of your changes

### Commit Messages

- Use clear, descriptive commit messages
- Start with a verb in imperative mood (e.g., "Add", "Fix", "Update")
- Reference related issues where applicable

### Developer Certificate of Origin

By submitting a contribution, you certify that you have the right to submit it under the BSD-3-Clause license. You may optionally add a `Signed-off-by` line to your commits:

```
Signed-off-by: Your Name <your.email@example.com>
```

## Code Standards

### Required Reading

Before contributing code, please read:
- [`doc/writing-secure-code.md`](doc/writing-secure-code.md) - **Mandatory** security guidelines
- [`AGENTS.md`](AGENTS.md) - Memory management and coding conventions

### Key Requirements

- **Memory management**: Use the arena allocator exclusively; never use raw `malloc`/`free`
- **String safety**: No `strcpy`, `strcat`, `sprintf` - use bounded alternatives
- **Error handling**: Always check return values; fail closed on errors
- **Input validation**: All external inputs must be bounded and validated
- **No compiler warnings**: Code must compile cleanly with `-Wall -Werror`

### Testing

- Add tests for new functionality
- Ensure existing tests continue to pass
- Test on multiple platforms: macOS, Linux

## Reporting Issues

### Bugs

Open a GitHub issue with:
- Steps to reproduce
- Expected vs actual behavior
- Your platform and elm-wrap version (`wrap version`)

### Security Vulnerabilities

**Do not open public issues for security vulnerabilities.**

Please see [`SECURITY.md`](SECURITY.md) for responsible disclosure instructions.

## Questions?

Open a GitHub issue for general questions or discussions about the project.
