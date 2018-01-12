/*
clang -O2 cab/gunzip.c
*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void fatal(const char* msg, ...) {
  va_list args;
  va_start(args, msg);  // FIXME: add to clang's lib/clang/Basic/Builtins.def?
  vfprintf(stderr, msg, args);
  va_end(args);
  exit(1);
}

uint16_t little_uint16t(uint8_t* d) {
  return d[1] << 8 | d[0];
}

uint32_t little_uint32t(uint8_t* d) {
  return d[3] << 24 | d[2] << 16 | d[1] << 8 | d[0];
}

// rfc1951 describes the deflate bitstream.
struct Bitstream {
  int curbit;
  size_t curword;
  uint16_t curword_val;
  uint8_t* source;
  size_t source_len;
};

void bitstream_init(struct Bitstream* bs, uint8_t* source, size_t source_len) {
  bs->curbit = 0;
  bs->curword = 0;
  bs->curword_val = little_uint16t(source);
  bs->source = source;
  bs->source_len = source_len;
}

int getbit(struct Bitstream* bs) {
  int bit = (bs->curword_val >> bs->curbit) & 1;
  bs->curbit += 1;
  if (bs->curbit > 15) {
    bs->curbit = 0;
    bs->curword += 2;  // in bytes
    if (bs->curword < bs->source_len)
      bs->curword_val = little_uint16t(bs->source + bs->curword);
  }
  return bit;
}

int getbits(struct Bitstream* bs, int n) {
  // Doing this bit-by-bit is inefficient; this should try to bunch things up.
  int bits = 0;
  for (int i = 0; i < n; ++i)
    // deflate orders bits right-to-left.
    bits = bits | (getbit(bs) << i);
  return bits;
}

struct HuffTree {
};

void init_hufftree(struct HuffTree* ht, int* nodelengths, int nodecount) {
  // XXX
}

int readsym(struct HuffTree* ht, struct Bitstream* bs) {
  // XXX
  return 0;
}

void deflate_decode_pretree(struct HuffTree* pretree,
                            struct Bitstream* bitstream,
                            int* lengths,
                            int num_lengths) {
  int i = 0;
  while (i < num_lengths) {
    int code = readsym(pretree, bitstream);
    // code 0-15: Len[x] = code
    // 16: for next (3 + getbits(2)) elements, Len[x] = previous code
    // 17: for next (3 + getbits(3)) elements, Len[x] = 0
    // 18: for next (11 + getbits(7)) elements, Len[x] = 0
    if (code <= 15) {
      lengths[i] = code;
      i += 1;
    } else if (code == 16) {
      int n = 3 + getbits(bitstream, 2);
      for (int j = i; j < i+n; ++j)
        lengths[j] = lengths[i - 1];
      i += n;
    } else if(code == 17) {
      int n = 3 + getbits(bitstream, 3);
      for (int j = i; j < i+n; ++j)
        lengths[j] = 0;
      i += n;
    } else {
      // code == 18
      int n = 11 + getbits(bitstream, 7);
      for (int j = i; j < i+n; ++j)
        lengths[j] = 0;
      i += n;
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc <= 1)
    fatal("need filename\n");
  FILE* in = fopen(argv[1], "rb");
  if (!in)
    fatal("failed to open %s\n", argv[1]);

  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  rewind(in);

  uint8_t* gz = (uint8_t*)malloc(size);
  fread(gz, size, 1, in);

  if (size < 10) fatal("file too small\n");

  if (memcmp("\x1f\x8b", gz, 2)) fatal("invalid file header\n");
  if (gz[2] != 8) fatal("unexpected compression method %d\n", gz[2]);
  uint8_t flags = gz[3];
  time_t mtime = little_uint32t(gz + 4);
  uint8_t extra_flags = gz[8];
  uint8_t os = gz[9];
  size_t off = 10;

  printf("flags %d\n", flags);
  printf("mtime %s", ctime(&mtime));  // ctime() result contains trailing \n
  printf("extra_flags %d\n", extra_flags);
  printf("os %d\n", os);

  if (flags & 4) {
    uint32_t extra_size = little_uint32t(gz + off);
    printf("extra size %d\n", extra_size);
    off += 4 + extra_size;
  }

  if (flags & 8) {
    printf("name %s\n", gz + off);
    off += strlen((char*)gz + off) + 1;
  }

  if (flags & 16) {
    printf("comment %s\n", gz + off);
    off += strlen((char*)gz + off) + 1;
  }

  if (flags & 2) {
    printf("header crc16 %d\n", little_uint16t(gz + off));
    off += 2;
  }

  // XXX explain.
  int extra_len_bits[7*4 + 1] = {};
  for (int i = 0; i < 6*4; ++i)
    extra_len_bits[i + 4] = i/4;
  extra_len_bits[7*4] = 0;
  int base_length = 3;
  int base_lengths[7*4 + 1];
  for (int i = 0; i < 7*4 + 1; ++i) {
    base_lengths[i] = base_length;
    base_length += 1 << extra_len_bits[i];
  }
  // Code 285 is special: It's a 0-extra-bits encoding of the longest-possible
  // value and it means "258", which could also (less efficiently) be coded
  // as code 284 (base position 227) + 31 in the 5 extra bits. The construction
  // in the loop above would assign 259 to code 285 instead.
  base_lengths[7*4] = 258;
  int extra_dist_bits[30] = {};
  for (int i = 0; i < 28; ++i)
    extra_dist_bits[i + 2] = i/2;
  int base_dist = 1;
  int base_dists[30];
  for (int i = 0; i < 30; ++i) {
    base_dists[i] = base_dist;
    base_dist += 1 << extra_dist_bits[i];
  }

  struct Bitstream bitstream;
  bitstream_init(&bitstream, gz + off, size - off - 8);
  bool is_last_block;
  do {
    is_last_block = getbit(&bitstream);
    int block_type = getbits(&bitstream, 2);
    if (block_type == 3) fatal("invalid block\n");
    if (block_type == 0) fatal("unsupported uncompressed block\n");

    int lengths[288 + 30];
    int num_literals_lengths;
    int num_distances;
    if (block_type == 2) {
      // dynamic huffman code, read huffman tree description
      num_literals_lengths = getbits(&bitstream, 5) + 257;
      num_distances = getbits(&bitstream, 5) + 1;
      int num_pretree = getbits(&bitstream, 4) + 4;
      int pretree_order[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
      int pretree_lengths[19], i;
      for (i = 0; i < num_pretree; ++i)
        pretree_lengths[pretree_order[i]] = getbits(&bitstream, 3);
      for (; i < 19; ++i)
        pretree_lengths[pretree_order[i]] = 0;
 
      struct HuffTree pretree;
      init_hufftree(&pretree, pretree_lengths, 19);
      // "The code length repeat codes can cross from HLIT + 257 to the HDIST +
      // 1 code lengths", so we have to use a single list for the huflengths
      // here.
      deflate_decode_pretree(&pretree, &bitstream, lengths,
                             num_literals_lengths + num_distances);
    } else {
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      num_literals_lengths = 288;
      for (; i < 288 + 30; ++i) lengths[i] = 5;
      num_distances = 30;
    }
    struct HuffTree littree;
    init_hufftree(&littree, lengths, num_literals_lengths);
    struct HuffTree disttree;
    init_hufftree(&disttree, lengths + num_literals_lengths, num_distances);

    int code;
    do {
      code = readsym(&littree, &bitstream);
      if (code < 256) {
        // literal
        //window.output_literal(code)
      } else {
        // match. codes 257..285 represent lengths 3..258 (hence some bits might
        // have to follow the mapped code).
        code -= 257;
        int match_len =
            base_lengths[code] + getbits(&bitstream, extra_len_bits[code]);
        int dist = readsym(&disttree, &bitstream);
        int match_offset =
          base_dists[dist] + getbits(&bitstream, extra_dist_bits[dist]);
        //window.copy_match(match_offset, match_len)
      }
    } while (code != 256);
  } while (!is_last_block);

  free(gz);
  fclose(in);
}
