# tree-sitter and its grammars, vendored

kb reads source code through tree-sitter (`src/syntax.c`). The runtime and
every grammar below are compiled into kb statically; nothing is loaded at
runtime and nothing needs to be installed.

| directory | upstream | version | commit | license |
|---|---|---|---|---|
| `tree-sitter/` | https://github.com/tree-sitter/tree-sitter | v0.25.10 | `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406` | MIT |
| `tree-sitter-c/` | https://github.com/tree-sitter/tree-sitter-c | v0.24.2 | `b780e47fc780ddc8da13afa35a3f4ed5c157823d` | MIT |
| `tree-sitter-typescript/` (TypeScript and TSX) | https://github.com/tree-sitter/tree-sitter-typescript | v0.23.2 | `f975a621f4e7f532fe322e13c4f79495e0a7b2e7` | MIT |
| `tree-sitter-javascript/` (with JSX) | https://github.com/tree-sitter/tree-sitter-javascript | v0.25.0 | `44c892e0be055ac465d5eeddae6d3e194424e7de` | MIT |
| `tree-sitter-python/` | https://github.com/tree-sitter/tree-sitter-python | v0.25.0 | `293fdc02038ee2bf0e2e206711b69c90ac0d413f` | MIT |
| `tree-sitter-go/` | https://github.com/tree-sitter/tree-sitter-go | v0.25.0 | `1547678a9da59885853f5f5cc8a99cc203fa2e2c` | MIT |
| `tree-sitter-rust/` | https://github.com/tree-sitter/tree-sitter-rust | v0.24.2 | `77a3747266f4d621d0757825e6b11edcbf991ca5` | MIT |
| `tree-sitter-asm/` | https://github.com/RubixDev/tree-sitter-asm | v0.24.0 | `5bb5b03e3c1ce5853b5282b9fba060f7c7bbf11e` | MIT |

Each directory keeps its upstream `LICENSE`. The runtime also carries a few
ICU headers under `tree-sitter/lib/src/unicode/`, with ICU's own license
beside them.

## What was taken

- The runtime: `lib/include/` and `lib/src/` whole. Only `lib/src/lib.c` is
  compiled; it includes the other `.c` files.
- Each grammar: its generated `src/parser.c`, its `src/scanner.c` where it has
  one, the `src/tree_sitter/` headers, and `queries/tags.scm` where it has
  one (the definitions a code index cares about). Nothing is regenerated:
  `grammar.js` and the Node tooling stay upstream.
- TypeScript: both `typescript/` and `tsx/`, and `common/scanner.h`, which
  both scanners include.

**One local change:** `tree-sitter-typescript/common/tree_sitter/` holds a
copy of the (identical) `parser.h`, `alloc.h` and `array.h` from
`typescript/src/tree_sitter/`. Upstream's build finds them through an include
path; kb compiles each file on its own, and `common/scanner.h` looks for them
beside itself.

## Why this assembly grammar

RubixDev's grammar is one grammar for many assemblers rather than one per
dialect: GNU as in AT&T and Intel syntax, NASM-style sources, ARM, RISC-V and
MIPS all parse with only a few percent of their bytes in error nodes, and
labels come out as `label` nodes. A dialect-specific grammar (NASM, x86 GAS)
could be added beside it if one dialect needs a finer tree.

## Size

The generated parsers are about 35 MB of C, most of it tables; the whole set
compiles in about three seconds. kb's binary grows from 0.6 MB to 6.6 MB.

## Updating

Replace a grammar's files from a tagged release, update its row above, and
run `make test` and `bin/kb-syntax-dump` over the repository's own code
(`make bin/kb-syntax-dump`): every file should still come out as `tree`. The
runtime accepts grammars of ABI 13 to 15; a grammar generated for a newer ABI
needs a newer runtime.
