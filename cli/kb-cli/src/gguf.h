/* GGUF container reader (§8: "weights load from safetensors or GGUF").
 *
 * WHAT A GGUF FILE IS. A header, a block of key/value metadata, a directory
 * of tensors, then — after padding to the alignment the metadata itself
 * names — the tensor data. Every integer is little-endian. Every string is a
 * 64-bit length followed by unterminated UTF-8 bytes.
 *
 * THE FILE DESCRIBES ITSELF AND THIS READER BELIEVES ONLY THE FILE. The
 * architecture, the layer count, the embedding width, the tokenizer and its
 * whole vocabulary are all in the metadata. Nothing here hard-codes a model:
 * `gguf_open` gets you the key/value block and the tensor directory, and it
 * is the caller's job (embed.c) to decide whether what it found is a model
 * it can run.
 *
 * REFUSAL IS THE ONLY ALTERNATIVE TO UNDERSTANDING. A file whose magic,
 * version, alignment, counts or offsets this reader cannot make sense of is
 * rejected with a sentence saying which one — never half-read. The whole
 * point of the format check is that the bytes after it are trusted as
 * pointers into a mapping, and a half-read header is a wild read waiting to
 * happen. So every length is compared against the mapped size BEFORE it is
 * used, including the ones that are only ever added together, and every
 * tensor's computed byte extent is checked to lie inside the data section.
 *
 * THE FILE IS MAPPED, NOT COPIED. Weights are read by the forward pass over
 * and over and never written; see plat_map_file.
 */
#ifndef KB_GGUF_H
#define KB_GGUF_H

#include "platform.h"
#include "str.h"

/* Metadata value types, as numbered by the format. */
typedef enum {
    GGUF_U8 = 0,
    GGUF_I8 = 1,
    GGUF_U16 = 2,
    GGUF_I16 = 3,
    GGUF_U32 = 4,
    GGUF_I32 = 5,
    GGUF_F32 = 6,
    GGUF_BOOL = 7,
    GGUF_STRING = 8,
    GGUF_ARRAY = 9,
    GGUF_U64 = 10,
    GGUF_I64 = 11,
    GGUF_F64 = 12,
    GGUF_TYPE_COUNT = 13
} GgufType;

/* Tensor element types. Only the ones kb can actually read are named; any
 * other number is carried through so an error can state it. */
typedef enum {
    GGML_F32 = 0,
    GGML_F16 = 1,
    GGML_Q4_K = 12,
    GGML_Q5_K = 13,
    GGML_Q6_K = 14
} GgmlType;

/* Tensor dimensions are stored innermost-first: ne[0] is the length of a
 * ROW and is contiguous in memory; ne[1] is how many rows there are. A
 * weight printed as [768, 2304] is 2304 rows of 768 values, and a matrix
 * product against it is 2304 dot products of length 768. */
#define GGUF_MAX_DIMS 4

typedef struct {
    const char *key; /* NUL-terminated, arena-owned */
    GgufType type;
    GgufType elem;   /* element type when type is GGUF_ARRAY */
    uint64_t count;  /* elements when type is GGUF_ARRAY, else 1 */
    const uint8_t *payload; /* first element, into the mapping */
    size_t payload_len;
    /* Decoded scalar, for a non-array value. A number is decoded into all
     * three so a caller can ask for the shape it wants without knowing which
     * width the file happened to use. `s` is NULL unless type is STRING. */
    uint64_t u;
    int64_t i;
    double f;
    const char *s;
} GgufKV;

typedef struct {
    const char *name;
    uint32_t n_dims;
    uint64_t ne[GGUF_MAX_DIMS];
    uint32_t type; /* a GgmlType, or a number this build does not know */
    uint64_t offset;
    const uint8_t *data; /* resolved and bounds-checked */
    uint64_t bytes;
} GgufTensor;

typedef struct {
    PlatMap *map;
    const uint8_t *base;
    size_t len;
    uint32_t version;
    uint64_t alignment;
    GgufKV *kv;
    uint64_t n_kv;
    GgufTensor *tensors;
    uint64_t n_tensors;
    uint64_t data_off; /* where the tensor data section starts */
} Gguf;

/* Maps and validates. On failure *err says which check failed and nothing is
 * mapped. Arena allocations for the directories survive; the mapping does
 * not and must be released with gguf_close. */
bool gguf_open(Arena *a, const char *path, Gguf *out, char *err, size_t errsz);
void gguf_close(Gguf *g);

const GgufKV *gguf_find(const Gguf *g, const char *key);
/* Typed reads with a default. A key of the wrong kind reads as the default
 * rather than as garbage: metadata is written by other people's tools. */
uint64_t gguf_u64(const Gguf *g, const char *key, uint64_t def);
double gguf_f64(const Gguf *g, const char *key, double def);
const char *gguf_str(const Gguf *g, const char *key, const char *def);
bool gguf_bool(const Gguf *g, const char *key, bool def);

/* Decodes an array of strings into `out` (arena-owned `Str` views into the
 * mapping, so the strings live exactly as long as the mapping does). Returns
 * false when the key is absent or is not an array of strings. */
bool gguf_str_array(Arena *a, const Gguf *g, const char *key, Str **out,
                    uint64_t *n);
/* Decodes an array of 32-bit integers (signed or unsigned) into int32_t. */
bool gguf_i32_array(Arena *a, const Gguf *g, const char *key, int32_t **out,
                    uint64_t *n);

const GgufTensor *gguf_tensor(const Gguf *g, const char *name);
/* "blk.<i>.<suffix>", the naming every llama.cpp-converted model uses. */
const GgufTensor *gguf_layer_tensor(const Gguf *g, uint32_t layer,
                                    const char *suffix);

const char *gguf_type_name(uint32_t ggml_type);

#endif /* KB_GGUF_H */
