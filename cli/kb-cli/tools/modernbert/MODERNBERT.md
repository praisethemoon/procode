# ModernBERT in kb: the facts

What kb's C implementation of gte-modernbert-base (the embedder) and
gte-reranker-modernbert-base (the reranker) must reproduce. Each fact here was
read from the models' own files or from transformers' `modeling_modernbert.py`
(transformers 4.57.6), not from memory.

## Sources and files

| model | Hugging Face revision | kb file (F16) | SHA-256 of the kb file |
|---|---|---|---|
| Alibaba-NLP/gte-modernbert-base | `e7f32e3c00f91d699e8c43b53106206bcc72bb22` | `gte-modernbert-base.F16.gguf` (299,876,256 bytes) | `2587e31accb3a0f107f57baa2a4bfc60d1a9173b0942d393d6a9c678cb8a12b3` |
| Alibaba-NLP/gte-reranker-modernbert-base | `f7481e6055501a30fb19d090657df9ec1f79ab2c` | `gte-reranker-modernbert-base.F16.gguf` (301,060,800 bytes) | `f13979995522155e092ce9287b2b41943c4f91d7daf8e9d988fd90bce1bcb196` |

Both are Apache-2.0. The files live in `~/.kb/models/` (§8 of index-api.md).
To reproduce them:

```sh
python -m venv venv && venv/bin/pip install torch transformers sentence-transformers gguf safetensors numpy
venv/bin/python -c "from huggingface_hub import snapshot_download as d; print(d('Alibaba-NLP/gte-modernbert-base', revision='e7f32e3c00f91d699e8c43b53106206bcc72bb22'))"
venv/bin/python convert.py <that snapshot> gte-modernbert-base.F16.gguf --revision e7f32e3c00f91d699e8c43b53106206bcc72bb22 --name gte-modernbert-base
# and the same for the reranker at f7481e6055501a30fb19d090657df9ec1f79ab2c
```

The conversion is deterministic: the same revision gives the same bytes.
`reference.py` writes the outputs the C code is tested against; the compact
copy kb's tests read is `tests/fixtures/modernbert/reference.json`.

## Tokenizer (identical ids for both models)

- **Byte-level BPE** in the GPT-2 family: 50,280 vocabulary entries and 50,009
  merges, no byte fallback, no `[UNK]` produced in practice. The model's
  embedding table has 50,368 rows; ids above the vocabulary are added tokens.
- **Normaliser:** NFC.
- **Pre-tokeniser:** ByteLevel with the GPT-2 split regex, no prefix space:
  `'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+`.
- **Added tokens come first.** Before any regex or BPE, the (normalised) text is
  split on the 116 added tokens, longest match first. The matches become one id
  each, and only the text between them goes through BPE. They are:
  - runs of **2 to 24 spaces** (ids 50254–50276), which is what indentation in code becomes;
  - the placeholders `|||IP_ADDRESS|||` (0), `|||EMAIL_ADDRESS|||` (50277), `|||PHONE_NUMBER|||` (50278);
  - `[unused0]`–`[unused82]` (50285–50367);
  - the specials: `<|padding|>` 1, `<|endoftext|>` 50279, `[UNK]` 50280, `[CLS]` 50281, `[SEP]` 50282, `[PAD]` 50283, `[MASK]` 50284.
  They are `normalized: true` and neither lstrip nor rstrip.
- **Template:** `[CLS] A [SEP]` for one text, `[CLS] A [SEP] B [SEP]` for a
  pair. The reranker's pair is (query, passage).
- **Length:** the models accept 8,192 positions (the reranker's own tokenizer
  truncates at 8,000); kb sets its own budget per chunk.

## Encoder (both models)

22 layers, hidden size 768, 12 heads of 64, MLP inner size 1152. There are no
biases anywhere in the encoder (attention, MLP or norms). Every norm is
LayerNorm without bias, with eps 1e-5: `y = (x − mean) / sqrt(var + eps) · w`.

1. `x = LayerNorm_emb(E[ids])`, with no position embeddings (positions come
   from RoPE).
2. For layer `i` from 0 to 21:
   - `x = x + Attn_i(norm_attn_i(x))`. **Layer 0 has no attention norm** (it is
     an identity; there is no `blk.0.attn_norm.weight`).
   - `x = x + Wo_mlp · (GELU(u) ⊙ g)`, where `[u ‖ g] = Wi · norm_mlp_i(x)`:
     `Wi` is 2304×768, and the **first** half is activated, the second is the
     gate. GELU is the exact erf form.
3. `h = LayerNorm_final(x)`.

**Attention.** `qkv = Wqkv · x` (2304 = q ‖ k ‖ v, each 12 heads × 64). RoPE is
applied to q and k, and scores are `q·kᵀ / 8` (1/√64), then softmax and `Wo`.
- **Global layers** are those where `i % 3 == 0` (0, 3, …, 21). They attend to
  all tokens, with RoPE base 160,000.
- **Local layers** are the rest. They attend only where `|i − j| ≤ 64` (the
  128-token window halved each way), with RoPE base 10,000.
- **RoPE** is NeoX style (`rotate_half`): for dimension pair `d` in 0..31,
  `θ_d = base^(−2d/64)`, and position `p` rotates `(x[d], x[d+32])` by `p·θ_d`.

## Heads

- **Embedder:** the **CLS** token of `h` (`h[0]`), 768 dimensions, not
  normalised by the model (kb normalises for cosine). No query or document
  prefixes.
- **Reranker:**
  - pooling is the **mean** of `h` over the tokens (`classifier_pooling: mean`);
  - then `z = LayerNorm_cls(GELU(W_dense · pooled))`, where `W_dense` is 768×768 without a bias;
  - then `logit = w_out · z + b_out`, where `w_out` is 1×768 and there is one bias.

  A higher logit means more relevant. sentence-transformers applies a sigmoid
  by default; the reference stores the raw logit.

## GGUF layout (written by convert.py)

The architecture is `modernbert`. The keys are:

- `modernbert.context_length`, `.embedding_length`, `.block_count`,
  `.feed_forward_length`, `.attention.head_count`,
  `.attention.layer_norm_epsilon`;
- `.attention.sliding_window` (128) and `.attention.global_every` (3);
- `.rope.freq_base` (160000, global) and `.rope.freq_base_local` (10000);
- `.hidden_activation`, `.pooling` (`cls` or `mean`), and
  `.classifier_activation` (reranker only);
- `kb.role` (`embedder` or `reranker`), `kb.source.revision`,
  `kb.source.weights_sha256`, `kb.tokenizer.normalizer`;
- `tokenizer.ggml.model` = `gpt2`, `.pre` = `gpt2`, `.tokens` (50,368; an added
  token is stored as its raw text), `.token_type` (1 normal, 3 control,
  4 user-defined), `.merges`, and the cls, sep, pad, unk and mask ids.

The tensors are F16, except that every norm weight and the classifier bias are
F32. Shapes are in ggml order `[in, out]`.

| tensor | from |
|---|---|
| `token_embd.weight` `[768, 50368]` | `embeddings.tok_embeddings.weight` |
| `token_embd_norm.weight` | `embeddings.norm.weight` |
| `blk.i.attn_norm.weight` (i ≥ 1) | `layers.i.attn_norm.weight` |
| `blk.i.attn_qkv.weight` `[768, 2304]` | `layers.i.attn.Wqkv.weight` |
| `blk.i.attn_output.weight` `[768, 768]` | `layers.i.attn.Wo.weight` |
| `blk.i.ffn_norm.weight` | `layers.i.mlp_norm.weight` |
| `blk.i.ffn_up.weight` `[768, 2304]` | `layers.i.mlp.Wi.weight` |
| `blk.i.ffn_down.weight` `[1152, 768]` | `layers.i.mlp.Wo.weight` |
| `output_norm.weight` | `final_norm.weight` |
| `cls.dense.weight`, `cls.norm.weight`, `cls.output.weight`, `cls.output.bias` | reranker `head.dense`, `head.norm`, `classifier` |

## Reference values worth knowing

- Embedding norms before normalisation are ≈37–39.
- Reranker logits on the fixture's pairs:
  - +1.84 for a crash question against the crash-safety section, −1.91 against `.lapignore`;
  - +1.48 for "files the recorder should not track" against `.lapignore`.
