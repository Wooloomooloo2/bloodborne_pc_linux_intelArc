"""BLAKE3 (default hash mode), after the reference implementation in the BLAKE3 specification."""
import struct

IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
PERM = [2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8]
CHUNK_START, CHUNK_END, PARENT, ROOT = 1, 2, 4, 8
BLOCK_LEN, CHUNK_LEN = 64, 1024
M = 0xFFFFFFFF


def rotr(x, n):
    return ((x >> n) | (x << (32 - n))) & M


def g(s, a, b, c, d, x, y):
    s[a] = (s[a] + s[b] + x) & M
    s[d] = rotr(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & M
    s[b] = rotr(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b] + y) & M
    s[d] = rotr(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & M
    s[b] = rotr(s[b] ^ s[c], 7)


def compress(cv, block_words, counter, block_len, flags):
    s = list(cv) + IV[:4] + [counter & M, (counter >> 32) & M, block_len, flags]
    m = list(block_words)
    for r in range(7):
        g(s, 0, 4, 8, 12, m[0], m[1]); g(s, 1, 5, 9, 13, m[2], m[3])
        g(s, 2, 6, 10, 14, m[4], m[5]); g(s, 3, 7, 11, 15, m[6], m[7])
        g(s, 0, 5, 10, 15, m[8], m[9]); g(s, 1, 6, 11, 12, m[10], m[11])
        g(s, 2, 7, 8, 13, m[12], m[13]); g(s, 3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[PERM[i]] for i in range(16)]
    for i in range(8):
        s[i] ^= s[i + 8]
        s[i + 8] ^= cv[i]
    return s


def words(block):
    block = block + b"\0" * (BLOCK_LEN - len(block))
    return list(struct.unpack("<16I", block))


class Output:
    def __init__(self, cv, block_words, counter, block_len, flags):
        self.cv, self.bw, self.counter, self.block_len, self.flags = cv, block_words, counter, block_len, flags

    def chaining_value(self):
        return compress(self.cv, self.bw, self.counter, self.block_len, self.flags)[:8]

    def root_bytes(self, n=32):
        out = compress(self.cv, self.bw, 0, self.block_len, self.flags | ROOT)
        return struct.pack("<16I", *out)[:n]


def chunk_output(chunk, counter):
    cv = list(IV)
    blocks = [chunk[i:i + BLOCK_LEN] for i in range(0, len(chunk), BLOCK_LEN)] or [b""]
    for i, b in enumerate(blocks):
        flags = (CHUNK_START if i == 0 else 0) | (CHUNK_END if i == len(blocks) - 1 else 0)
        if i == len(blocks) - 1:
            return Output(cv, words(b), counter, len(b), flags)
        cv = compress(cv, words(b), counter, BLOCK_LEN, flags)[:8]


def parent_output(left, right):
    return Output(list(IV), left + right, 0, BLOCK_LEN, PARENT)


def blake3(data: bytes, n=32) -> bytes:
    chunks = [data[i:i + CHUNK_LEN] for i in range(0, len(data), CHUNK_LEN)] or [b""]
    stack = []  # chaining values, merged like the reference's add_chunk_chaining_value
    for counter, chunk in enumerate(chunks[:-1]):
        cv = chunk_output(chunk, counter).chaining_value()
        total = counter + 1
        while total & 1 == 0:
            cv = parent_output(stack.pop(), cv).chaining_value()
            total >>= 1
        stack.append(cv)
    out = chunk_output(chunks[-1], len(chunks) - 1)
    while stack:
        out = parent_output(stack.pop(), out.chaining_value())
    return out.root_bytes(n)


if __name__ == "__main__":
    import sys
    assert blake3(b"").hex() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
    for p in sys.argv[1:]:
        print(blake3(open(p, "rb").read()).hex(), p)
