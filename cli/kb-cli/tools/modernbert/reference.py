#!/usr/bin/env python3
"""Reference outputs for kb's ModernBERT port, from the Hugging Face models
themselves: token ids, embeddings, reranker scores, and one text's hidden
state after every layer (the first place a port goes wrong is the first
layer that disagrees).

    python reference.py <embedder-snapshot> <reranker-snapshot> <out.json>

Needs torch, transformers and sentence-transformers. Runs on CPU in float32,
which is what kb computes in.
"""

import json
import sys

import torch
from sentence_transformers import CrossEncoder, SentenceTransformer
from transformers import AutoModel, AutoTokenizer

TEXTS = [
    "IOCP delivers completions on a port that worker threads wait on.",
    "What happens if the program crashes halfway through writing?",
    "static bool store_is_store(const char *parent, const char *probe) {\n    char home[KB_PATH_MAX];\n\treturn false;\n}\n",
    "def chunk(text: str, budget: int = 400) -> list[str]:\n        return [text[i:i + budget] for i in range(0, len(text), budget)]\n",
    "export function query(log: LapLog, filter: HistoryFilter): HistoryPage {}",
    "Ünïcödé, naïve café — «quotes», 日本語のテキスト, emoji 🚀✨ and CJK 中文.",
    "Contact me at someone@example.com or 192.168.0.1 about [unused3] and <|endoftext|>.",
    "mov rax, [rbp-8]\n    call _main\n    ret",
    "   leading spaces, trailing spaces   \n\n\n  and blank lines\r\n",
    "",
    " ".join(["token"] * 600),
]

PAIRS = [
    ["what happens if the program crashes halfway through writing",
     "## Concurrency & crash safety\nA writer takes the lock, appends one record and fsyncs; a torn tail is repaired by the next writer."],
    ["what happens if the program crashes halfway through writing",
     "## `.lapignore`\ngitignore subset: `#` comments; trailing `/` = directories only."],
    ["files the recorder should not track",
     "## `.lapignore`\ngitignore subset: `#` comments; trailing `/` = directories only; always ignored: .lap/, .git/."],
    ["how to find a store", "store_find walks up from the working directory to the first .kb it finds."],
    ["kb_add", "The six tools: kb_search, kb_get, kb_add, kb_links, kb_collections, kb_stale."],
]


def main():
    emb_dir, rr_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
    torch.set_grad_enabled(False)
    tok = AutoTokenizer.from_pretrained(emb_dir)

    tokens = [tok(t)["input_ids"] for t in TEXTS]

    st = SentenceTransformer(emb_dir, device="cpu")
    st.max_seq_length = 8192
    vecs = st.encode(TEXTS, convert_to_numpy=True, normalize_embeddings=False, batch_size=1)

    model = AutoModel.from_pretrained(emb_dir, torch_dtype=torch.float32)
    model.eval()
    enc = tok(TEXTS[2], return_tensors="pt")
    hs = model(**enc, output_hidden_states=True).hidden_states
    layers = [h[0].tolist() for h in hs]  # embeddings output, then one per layer

    ce = CrossEncoder(rr_dir, device="cpu")
    logits = ce.predict(PAIRS, activation_fn=torch.nn.Identity(), batch_size=1).tolist()
    pair_tokens = [tok(q, p)["input_ids"] for q, p in PAIRS]

    json.dump(
        {
            "texts": TEXTS,
            "tokens": tokens,
            "embeddings": vecs.tolist(),
            "hidden_states_text": 2,
            "hidden_states": layers,
            "pairs": PAIRS,
            "pair_tokens": pair_tokens,
            "reranker_logits": logits,
        },
        open(out, "w"),
    )
    print(f"wrote {out}: {len(TEXTS)} texts, {len(PAIRS)} pairs, {len(layers)} hidden states")


if __name__ == "__main__":
    main()
