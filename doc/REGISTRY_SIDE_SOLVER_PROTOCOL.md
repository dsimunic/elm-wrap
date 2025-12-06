# Registry-Side Solver Protocol

## Overview

This document proposes a server-side dependency solver protocol for **elm-wrap** registries. Instead of implementing complex constraint solving on the client, the solver logic moves to the registry server, which can provide richer functionality, better performance, and centralized policy enforcement.

## Motivation

### Current Problem

The current major upgrade logic in `solver.c` allows any package version that the registry/provider exposes, without adding extra elm-version constraints in the solver itself:

```c
if (major_upgrade) {
  /* For major upgrades, allow any version */
  log_debug("Allowing major upgrades for all packages");
  /* Don't add any root constraints - let solver pick latest versions.
   * We rely on the registry/provider to expose only package versions that
   * are compatible with the current Elm compiler version (via ELM_HOME
   * being versioned per compiler), so the solver itself only ever sees
   * compatible packages here.
   */
}
```

With this design, elm-version compatibility is enforced at the registry/provider level (e.g. by maintaining a separate `registry.dat` per compiler version), and the client-side solver operates over an already filtered, compatible package universe.

### Registry Limitations

The canonical `all-packages` endpoint only provides package names and versions:

```json
{
  "author/package": ["1.0.0", "1.0.1", "2.0.0"]
}
```

It does not include elm-version constraints, requiring clients to download individual `elm.json` files for compatibility checking.

## Solution: Server-Side Solver

Move dependency resolution to the registry server. Clients send their current project state and desired operation; servers return exact install commands.

### Architecture Fit

This leverages **elm-wrap**'s hierarchical registry design, which supports extended protocols beyond the basic canonical registry. Registries can advertise solver capabilities via capability discovery.

## Protocol Specification

### 1. Capability Discovery

Registries advertise solver support:

```json
GET /capabilities
{
  "version": "2.0",
  "features": ["solver", "authentication", "full-source-storage"],
  "solver-endpoint": "/solve",
  "max-payload-size": 1048576
}
```

### 2. Solver Request

Clients send project state and operation:

```json
POST /solve
{
  "operation": "major-upgrade",
  "elm-version": "0.19.1",
  "current-dependencies": {
    "direct": {
      "elm/core": "1.0.4",
      "elm/html": "1.0.0"
    },
    "indirect": {
      "elm/virtual-dom": "1.0.2",
      "elm/json": "1.1.2"
    }
  },
  "test-dependencies": {
    "direct": {
      "elm-explorations/test": "1.0.0"
    },
    "indirect": {
      "elm/random": "1.0.0"
    }
  },
  "registry-since": 16446,
  "options": {
    "allow-pre-releases": false,
    "ignore-test-dependencies": false
  }
}
```

**Operation Types:**
- `"install-package"`: Install specific package
- `"major-upgrade"`: Allow major version upgrades
- `"minor-upgrade"`: Only minor/patch upgrades
- `"update-package"`: Update specific package
- `"check-updates"`: List available updates without applying

### 3. Solver Response

Server returns exact actions to perform:

```json
{
  "status": "success",
  "actions": [
    {
      "type": "install",
      "package": "elm/http",
      "version": "2.0.0",
      "reason": "major upgrade available"
    },
    {
      "type": "upgrade",
      "package": "elm/virtual-dom",
      "from": "1.0.2",
      "to": "1.0.5",
      "reason": "minor update"
    },
    {
      "type": "remove",
      "package": "deprecated-package",
      "version": "1.0.0",
      "reason": "no longer needed"
    }
  ],
  "new-elm-json": {
    "type": "application",
    "source-directories": ["src"],
    "elm-version": "0.19.1",
    "dependencies": {
      "direct": {
        "elm/core": "1.0.4",
        "elm/html": "1.0.0",
        "elm/http": "2.0.0"
      },
      "indirect": {
        "elm/virtual-dom": "1.0.5",
        "elm/json": "1.1.3",
        "elm/bytes": "1.0.8"
      }
    },
    "test-dependencies": {
      "direct": {
        "elm-explorations/test": "1.0.0"
      },
      "indirect": {
        "elm/random": "1.0.0"
      }
    }
  },
  "registry-updated": true,
  "new-registry-count": 16500,
  "warnings": [
    "Package elm/old-package was deprecated and removed"
  ]
}
```

**Error Response:**

```json
{
  "status": "error",
  "error-type": "no-solution",
  "message": "No compatible version of elm/http found for Elm 0.19.1",
  "conflicts": [
    {
      "package": "elm/http",
      "constraint": "2.0.0 <= v < 3.0.0",
      "reason": "requires Elm 0.19.2 or higher"
    }
  ],
  "suggestions": [
    "Consider upgrading to Elm 0.19.2 or later",
    "Use elm/http 1.0.0 which supports Elm 0.19.1"
  ]
}
```

## Extended Registry Format

To support efficient client-side solving when server-side solving is unavailable, extend the `all-packages` format:

```json
{
  "packages": {
    "elm/core": {
      "versions": ["1.0.0", "1.0.1", "1.0.2"],
      "elm-versions": {
        "1.0.0": "0.19.0 <= v < 0.20.0",
        "1.0.1": "0.19.0 <= v < 0.20.0",
        "1.0.2": "0.19.1 <= v < 0.20.0"
      },
      "dependencies": {
        "1.0.0": {},
        "1.0.1": {},
        "1.0.2": {}
      }
    },
    "elm/html": {
      "versions": ["1.0.0"],
      "elm-versions": {
        "1.0.0": "0.19.0 <= v < 0.20.0"
      },
      "dependencies": {
        "1.0.0": {
          "elm/core": "1.0.0 <= v < 2.0.0",
          "elm/virtual-dom": "1.0.0 <= v < 2.0.0"
        }
      }
    }
  },
  "total-count": 16500
}
```

## Benefits

### For Users
- **Automatic Compatibility**: No more elm-version mismatch errors
- **Better Error Messages**: Server provides detailed conflict explanations
- **Policy Enforcement**: Organizations can implement custom rules
- **Performance**: Server-side solving can be optimized

### For Registry Operators
- **Centralized Logic**: Update solver algorithms without client updates
- **Analytics**: Track solving patterns and popular packages
- **Policy Control**: Enforce organizational package policies
- **Resource Efficiency**: Servers can cache solving results

### For Developers
- **Simpler Clients**: Remove complex PubGrub implementation
- **Fallback Support**: Graceful degradation to client-side solving
- **Extensibility**: Easy to add new solving strategies

## Implementation Strategy

### Phase 1: Extended Registry Format
- Extend `all-packages` endpoint to include elm-version constraints
- Maintain backward compatibility with canonical registry
- Update client to use cached elm-version ranges

### Phase 2: Basic Server-Side Solver
- Implement `/solve` endpoint with current PubGrub logic
- Support basic operations (install, upgrade)
- Add capability discovery

### Phase 3: Advanced Features
- Policy engine for organizational rules
- Batch operations and dry-run mode
- Enhanced error reporting and suggestions

### Phase 4: Client Integration
- Detect solver-capable registries
- Prefer server-side solving when available
- Maintain client-side fallback for basic registries

## Backward Compatibility

- **Canonical Registry**: Continues to work with basic metadata-only format
- **Client Fallback**: Clients can fall back to local solving
- **Incremental Adoption**: Registries can opt into solver support
- **Version Negotiation**: Capability discovery allows feature detection

## Security Considerations

- **Input Validation**: Validate all client-provided elm.json data
- **Rate Limiting**: Prevent abuse of solver endpoint
- **Authentication**: Support authenticated requests for private registries
- **Audit Logging**: Track solving operations for compliance

## Performance Considerations

- **Caching**: Cache solving results for common dependency sets
- **Incremental Updates**: Use registry-since mechanism to avoid full downloads
- **Parallel Solving**: Server can distribute solving across multiple cores
- **Result Compression**: Compress large elm.json responses

## Future Extensions

- **Batch Operations**: Solve multiple projects simultaneously
- **Dependency Graphs**: Return visual dependency graphs
- **Impact Analysis**: Show which packages would be affected by changes
- **Policy DSL**: Allow registries to define custom constraint rules
- **Integration Testing**: Verify solutions against test suites

This protocol transforms **elm-wrap** into a comprehensive package management platform, enabling sophisticated enterprise package ecosystems while maintaining compatibility with the existing Elm ecosystem.