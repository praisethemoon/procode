#!/usr/bin/env python3
"""Convert a Hugging Face ModernBERT checkpoint (the embedder
gte-modernbert-base or the reranker gte-reranker-modernbert-base) into the
GGUF file kb loads.

    python convert.py <hf-snapshot-dir> <out.gguf> [--revision <sha>]

Needs `gguf`, `safetensors` and `numpy`. Weights are written as F16 (the
embedder ships in F16, so its conversion is exact; the reranker's F32 weights
are rounded to F16) and every norm weight as F32. Nothing is downloaded here:
the snapshot directory is whatever `huggingface_hub.snapshot_download` put on
disk, and its revision is recorded in the file so a model can be traced back.

The layout follows llama.cpp's names where llama.cpp has one, so the file
reads naturally next to kb's other GGUF; the keys under `kb.` are what kb
needs and llama.cpp has no name for. MODERNBERT.md beside this file is the
description of every key and tensor.
"""

import argparse
import hashlib
import json
import os
import sys

import numpy as np
from gguf import GGUFWriter, TokenType
from safetensors import safe_open

ARCH = "modernbert"


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def vocabulary(tok):
    """Every id from 0 to vocab_size-1 with its text and type. The BPE
    vocabulary holds byte-level strings; an added token (specials, the
    runs of spaces, the [unused] slots) is stored as the raw text it matches."""
    model = tok["model"]
    by_id = {i: (t, TokenType.NORMAL) for t, i in model["vocab"].items()}
    for a in tok["added_tokens"]:
        kind = TokenType.CONTROL if a["special"] else TokenType.USER_DEFINED
        by_id[a["id"]] = (a["content"], kind)
    return by_id


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("out")
    ap.add_argument("--revision", default="")
    ap.add_argument("--name", default="")
    args = ap.parse_args()

    cfg = load_json(os.path.join(args.snapshot, "config.json"))
    tok = load_json(os.path.join(args.snapshot, "tokenizer.json"))
    weights = os.path.join(args.snapshot, "model.safetensors")
    if cfg.get("model_type") != "modernbert":
        sys.exit(f"not a ModernBERT checkpoint: model_type={cfg.get('model_type')}")
    reranker = "ModernBertForSequenceClassification" in cfg.get("architectures", [])
    prefix = "model." if reranker else ""

    pooling = "cls"
    pool_cfg = os.path.join(args.snapshot, "1_Pooling", "config.json")
    if os.path.exists(pool_cfg):
        p = load_json(pool_cfg)
        pooling = "cls" if p.get("pooling_mode_cls_token") else "mean" if p.get("pooling_mode_mean_tokens") else "?"
    if reranker:
        pooling = cfg.get("classifier_pooling", "mean")

    n_vocab = cfg["vocab_size"]
    by_id = vocabulary(tok)
    tokens, types = [], []
    for i in range(n_vocab):
        text, kind = by_id.get(i, (f"[PAD{i}]", TokenType.UNUSED))
        tokens.append(text)
        types.append(int(kind))
    merges = [" ".join(m) if isinstance(m, list) else m for m in tok["model"]["merges"]]
    specials = {a["content"]: a["id"] for a in tok["added_tokens"] if a["special"]}

    name = args.name or os.path.basename(os.path.normpath(args.snapshot))
    w = GGUFWriter(args.out, ARCH)
    w.add_name(name)
    w.add_string("kb.role", "reranker" if reranker else "embedder")
    w.add_string("kb.source.revision", args.revision)
    w.add_string("kb.source.weights_sha256", sha256_of(weights))

    w.add_uint32(f"{ARCH}.context_length", cfg["max_position_embeddings"])
    w.add_uint32(f"{ARCH}.embedding_length", cfg["hidden_size"])
    w.add_uint32(f"{ARCH}.block_count", cfg["num_hidden_layers"])
    w.add_uint32(f"{ARCH}.feed_forward_length", cfg["intermediate_size"])
    w.add_uint32(f"{ARCH}.attention.head_count", cfg["num_attention_heads"])
    w.add_float32(f"{ARCH}.attention.layer_norm_epsilon", cfg["norm_eps"])
    w.add_uint32(f"{ARCH}.attention.sliding_window", cfg["local_attention"])
    w.add_uint32(f"{ARCH}.attention.global_every", cfg["global_attn_every_n_layers"])
    w.add_float32(f"{ARCH}.rope.freq_base", cfg["global_rope_theta"])
    w.add_float32(f"{ARCH}.rope.freq_base_local", cfg["local_rope_theta"])
    w.add_string(f"{ARCH}.hidden_activation", cfg["hidden_activation"])
    w.add_string(f"{ARCH}.pooling", pooling)
    if reranker:
        w.add_string(f"{ARCH}.classifier_activation", cfg["classifier_activation"])

    w.add_string("tokenizer.ggml.model", "gpt2")
    w.add_string("tokenizer.ggml.pre", "gpt2")
    w.add_string("kb.tokenizer.normalizer", tok["normalizer"]["type"] if tok.get("normalizer") else "")
    w.add_array("tokenizer.ggml.tokens", tokens)
    w.add_array("tokenizer.ggml.token_type", types)
    w.add_array("tokenizer.ggml.merges", merges)
    w.add_uint32("tokenizer.ggml.cls_token_id", specials["[CLS]"])
    w.add_uint32("tokenizer.ggml.seperator_token_id", specials["[SEP]"])
    w.add_uint32("tokenizer.ggml.padding_token_id", specials["[PAD]"])
    w.add_uint32("tokenizer.ggml.unknown_token_id", specials["[UNK]"])
    w.add_uint32("tokenizer.ggml.mask_token_id", specials["[MASK]"])

    renames = {
        "embeddings.tok_embeddings.weight": "token_embd.weight",
        "embeddings.norm.weight": "token_embd_norm.weight",
        "final_norm.weight": "output_norm.weight",
        "head.dense.weight": "cls.dense.weight",
        "head.norm.weight": "cls.norm.weight",
        "classifier.weight": "cls.output.weight",
        "classifier.bias": "cls.output.bias",
    }
    layer_parts = {
        "attn_norm.weight": "attn_norm.weight",
        "attn.Wqkv.weight": "attn_qkv.weight",
        "attn.Wo.weight": "attn_output.weight",
        "mlp_norm.weight": "ffn_norm.weight",
        "mlp.Wi.weight": "ffn_up.weight",
        "mlp.Wo.weight": "ffn_down.weight",
    }
    with safe_open(weights, "np") as f:
        for key in f.keys():
            k = key[len(prefix):] if key.startswith(prefix) else key
            if k.startswith("layers."):
                _, i, part = k.split(".", 2)
                name_out = f"blk.{i}.{layer_parts[part]}"
            elif k in renames:
                name_out = renames[k]
            else:
                sys.exit(f"unexpected tensor {key}")
            t = f.get_tensor(key)
            one_d = t.ndim == 1
            t = t.astype(np.float32 if one_d else np.float16)
            w.add_tensor(name_out, t)

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {args.out}: {name} ({'reranker' if reranker else 'embedder'}), pooling {pooling}")


if __name__ == "__main__":
    main()
