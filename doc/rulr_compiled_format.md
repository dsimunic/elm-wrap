# Rulr Compiled AST Format (.dlc)

This document describes the binary format used by `rulrc` to compile Datalog rule files (`.dl`) into a compressed binary representation (`.dlc`). The format is designed for fast loading at runtime while guaranteeing that syntax and semantic errors are caught at compile time.

## Overview

The compilation pipeline:

```
Source (.dl) → Lexer → Parser → AstProgram → Validation (IR) → Serialize → Compress → .dlc file
```

At runtime:

```
.dlc file → Decompress → Deserialize → AstProgram → IR Builder → Engine
```

## File Format

### Header (12 bytes, uncompressed)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | Magic | ASCII string `RULRAST1` |
| 8 | 4 | Size | Uncompressed payload size (little-endian uint32) |

### Compressed Payload

The payload is compressed using zlib/deflate (RFC 1951). Use `mz_compress()` / `mz_uncompress()` from miniz or equivalent zlib functions.

## Payload Structure (after decompression)

All multi-byte integers are little-endian. Strings are length-prefixed with a 16-bit length.

### String Encoding

```
string := length:u16 data:byte[length]
```

If `length == 0`, the string is NULL/empty.

### Flags (1 byte)

| Bit | Meaning |
|-----|---------|
| 0 | `clear_derived` - if set, `.clear_derived()` directive was present |
| 1-7 | Reserved (must be 0) |

### Declarations Section

```
num_decls: u16
decls: Decl[num_decls]
```

Each `Decl`:

```
name: string              % predicate name
arity: u8                 % number of arguments
args: DeclArg[arity]
```

Each `DeclArg`:

```
arg_name: string          % parameter name (may be NULL)
arg_type: string          % type name: "symbol", "int", or "range"
```

### Facts Section

```
num_facts: u16
facts: Fact[num_facts]
```

Each `Fact`:

```
pred: string              % predicate name
arity: u8                 % number of arguments
args: FactArg[arity]
```

Each `FactArg`:

```
kind: u8                  % 0 = string, 1 = int
value: string | i64       % depending on kind
```

### Rules Section

```
num_rules: u16
rules: Rule[num_rules]
```

Each `Rule`:

```
head_pred: string         % head predicate name
head_arity: u8
head_args: Term[head_arity]
num_body: u16
body: Literal[num_body]
```

### Term Encoding

```
kind: u8
```

| Kind | Value | Additional Data |
|------|-------|-----------------|
| VAR | 0 | `name: string` |
| STRING | 1 | `value: string` |
| INT | 2 | `value: i64` (8 bytes, little-endian) |
| WILDCARD | 3 | (none) |

### Literal Encoding

```
kind: u8
```

| Kind | Value | Structure |
|------|-------|-----------|
| POS | 0 | `pred: string`, `arity: u8`, `args: Term[arity]` |
| NEG | 1 | `pred: string`, `arity: u8`, `args: Term[arity]` |
| EQ | 2 | `lhs: Term`, `rhs: Term` |
| CMP | 3 | `cmp_op: u8`, `lhs: Term`, `rhs: Term` |
| BUILTIN | 4 | `builtin: u8`, `lhs: Term`, `rhs: Term` |

Comparison operators (`cmp_op`):

| Value | Operator |
|-------|----------|
| 0 | `=` |
| 1 | `!=` |
| 2 | `<` |
| 3 | `<=` |
| 4 | `>` |
| 5 | `>=` |

Builtin kinds (`builtin`):

| Value | Builtin |
|-------|---------|
| 0 | `match(pattern, string)` - regex match |

## Complete Binary Layout Example

For a simple rule file:

```datalog
.pred foo(x: symbol).
foo(X) :- bar(X).
```

The uncompressed payload would be:

```
[flags: 0x00]
[num_decls: 0x01 0x00]
  [name_len: 0x03 0x00] "foo"
  [arity: 0x01]
  [arg_name_len: 0x01 0x00] "x"
  [arg_type_len: 0x06 0x00] "symbol"
[num_facts: 0x00 0x00]
[num_rules: 0x01 0x00]
  [head_pred_len: 0x03 0x00] "foo"
  [head_arity: 0x01]
  [term_kind: 0x00] [var_name_len: 0x01 0x00] "X"
  [num_body: 0x01 0x00]
  [lit_kind: 0x00]
  [pred_len: 0x03 0x00] "bar"
  [arity: 0x01]
  [term_kind: 0x00] [var_name_len: 0x01 0x00] "X"
```

## Compilation Process

### 1. Parsing

The source file is tokenized and parsed into an `AstProgram` structure containing:
- Predicate declarations (`.pred name(arg: type, ...).`)
- Facts (`pred(value, ...).`)
- Rules (`head(...) :- body1(...), body2(...).`)

### 2. Validation

Before serialization, the AST is validated by building an IR (Intermediate Representation):

- **Type checking**: Argument types must match declarations
- **Arity checking**: Predicate uses must match declared arity
- **Safety checking**: Rule variables must appear in at least one positive literal
- **Stratification**: Rules with negation must be stratifiable (no negation cycles)

If validation fails, compilation aborts with an error message.

### 3. Serialization

The validated AST is serialized to a binary buffer using the format described above.

### 4. Compression

The binary buffer is compressed using zlib deflate at default compression level.

### 5. Output

The magic header, uncompressed size, and compressed data are written to the output file.

## Reading a .dlc File

Pseudocode for reading:

```
function read_dlc(path):
    data = read_file(path)
    
    # Check magic
    if data[0:8] != "RULRAST1":
        error("Invalid magic")
    
    # Read uncompressed size
    uncompressed_size = read_u32_le(data[8:12])
    
    # Decompress
    payload = zlib_decompress(data[12:], uncompressed_size)
    
    # Parse payload
    reader = ByteReader(payload)
    
    flags = reader.read_u8()
    clear_derived = (flags & 1) != 0
    
    num_decls = reader.read_u16()
    decls = []
    for i in range(num_decls):
        decls.append(read_decl(reader))
    
    num_facts = reader.read_u16()
    facts = []
    for i in range(num_facts):
        facts.append(read_fact(reader))
    
    num_rules = reader.read_u16()
    rules = []
    for i in range(num_rules):
        rules.append(read_rule(reader))
    
    return AstProgram(decls, facts, rules, clear_derived)
```

## Writing a .dlc File

Pseudocode for writing:

```
function write_dlc(ast, path):
    # Serialize to buffer
    writer = ByteWriter()
    
    flags = 1 if ast.clear_derived else 0
    writer.write_u8(flags)
    
    writer.write_u16(len(ast.decls))
    for decl in ast.decls:
        write_decl(writer, decl)
    
    writer.write_u16(len(ast.facts))
    for fact in ast.facts:
        write_fact(writer, fact)
    
    writer.write_u16(len(ast.rules))
    for rule in ast.rules:
        write_rule(writer, rule)
    
    payload = writer.get_bytes()
    
    # Compress
    compressed = zlib_compress(payload)
    
    # Write file
    output = ByteWriter()
    output.write_bytes("RULRAST1")
    output.write_u32_le(len(payload))
    output.write_bytes(compressed)
    
    write_file(path, output.get_bytes())
```

## rulrc Command Reference

```
rulrc compile [options] <file.dl>
rulrc compile --output <out.dlc>     # read from stdin
rulrc view <file.dlc>
rulrc [options] <path> [path ...]    # batch mode
```

### Commands

| Command | Description |
|---------|-------------|
| `compile <file.dl>` | Compile a single .dl file to .dlc |
| `compile --output <file>` | Compile from stdin to specified output |
| `view <file.dlc>` | Pretty-print compiled file in canonical format |
| `<path>` | Compile file(s) or all .dl files in directory |

### Options

| Option | Description |
|--------|-------------|
| `-o, --output <file>` | Specify output file path |
| `-v, --verbose` | Show compression statistics |
| `-h, --help` | Show help message |

### Examples

```bash
# Compile a single file
rulrc compile rules/my_rule.dl

# Compile with explicit output
rulrc compile rules/my_rule.dl --output build/my_rule.dlc

# Compile from stdin
cat rules/my_rule.dl | rulrc compile --output build/my_rule.dlc

# View compiled file
rulrc view build/my_rule.dlc

# Compile all files in a directory
rulrc rules/

# Round-trip test (view | compile should produce identical output)
rulrc view rule.dlc | rulrc compile --output rule2.dlc
diff rule.dlc rule2.dlc  # should be empty
```

## Source Files

| File | Purpose |
|------|---------|
| `src/rulr/frontend/ast.h` | AST data structures |
| `src/rulr/frontend/ast_serialize.h` | Serialization API |
| `src/rulr/frontend/ast_serialize.c` | Serialization implementation |
| `src/rulr/rulrc_main.c` | Compiler CLI |
| `src/vendor/miniz.h` | Compression library |

## Compression Ratios

Typical compression ratios for rule files:

| File Size | Compressed | Ratio |
|-----------|------------|-------|
| < 1 KB | ~150-300 bytes | 15-30% |
| 1-2 KB | ~200-500 bytes | 20-35% |

The format achieves good compression because:
1. Datalog files have repetitive predicate names
2. zlib/deflate excels at compressing text with repeated patterns
3. Binary encoding is more compact than text for integers
