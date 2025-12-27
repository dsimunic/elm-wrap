# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in elm-wrap, please report it responsibly.

### Preferred Method

Use [GitHub's private vulnerability reporting](https://github.com/dsimunic/elm-wrap/security/advisories/new) to submit your report. This keeps the details confidential until a fix is available.

### Alternative Contact

Email: damir@oomm.dev

Please include:
- A description of the vulnerability
- Steps to reproduce the issue
- Any relevant proof-of-concept code
- Your assessment of the potential impact

### Response Timeline

- **Acknowledgment**: Within 48 hours
- **Initial assessment**: Within 7 days
- **Fix timeline**: Depends on severity, typically within 30 days for critical issues

### What to Expect

1. We will acknowledge your report promptly
2. We will investigate and keep you informed of our progress
3. We will credit you in the security advisory (unless you prefer anonymity)
4. We will coordinate disclosure timing with you

## Supported Versions

| Version       | Supported          |
|---------------|--------------------|
| 0.6.x         | :white_check_mark: |
| < 0.6         | :x:                |

## Security Practices

This project follows secure coding practices documented in [`doc/writing-secure-code.md`](doc/writing-secure-code.md). Key practices include:

- Mandatory use of bounded memory allocations (arena allocator)
- Input size limits enforced via compile-time constants
- No use of unsafe string functions (strcpy, sprintf, etc.)
- Compiler hardening flags: `-Wall -Werror -Wunused-result`

## Known Security Considerations

elm-wrap processes untrusted input from:
- `elm.json` files in user projects
- Package metadata from Elm package registries
- Custom registry configurations

All inputs are validated and bounded. See [`doc/security-hardening.md`](doc/security-hardening.md) for our threat model and ongoing hardening efforts.
