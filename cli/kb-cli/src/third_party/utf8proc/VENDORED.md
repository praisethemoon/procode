# utf8proc, vendored

- Upstream: https://github.com/JuliaStrings/utf8proc
- Version: v2.10.0, commit `a1b99daa2a3393884220264c927a48ba1251a9c6` (2024-12-31), Unicode 16.0
- License: MIT (expat), see LICENSE.md
- Files: `utf8proc.c`, `utf8proc.h`, `utf8proc_data.c` (included by `utf8proc.c`), unmodified

kb uses it for two things the ModernBERT tokenizer (`src/bpe.c`) needs exactly
as the Hugging Face tokenizer does them: NFC normalisation, and the Unicode
general category of a codepoint (letters and numbers in the GPT-2 split
regex). It is compiled into kb statically; nothing is loaded at runtime.

To update: replace the three files from a tagged release, update the version
and commit above, and run `make test` plus the tokenizer check in
`tools/modernbert/check_tokenizer.py`: a newer Unicode can change tokens.
