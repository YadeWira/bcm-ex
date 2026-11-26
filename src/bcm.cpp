/*

BCM - A BWT-based file compressor

Copyright (C) 2008-2021 Ilya Muravyov

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

/*

bcm-ex - the extended line of BCM.

Modified work: based on BCM 1.65 by Ilya Muravyov
(https://github.com/FS-make-simple/bcm), changes Copyright (C) 2026 Yade Wira
Bravo. The list of changes is in the README and in the wiki
(History page); this file was changed as part of that work.

The compression model, the arithmetic coder and the stream layout are BCM
1.65's, unchanged: a file written here decodes with bcm 1.65 and the other way
round, and for the same input and block size both write the same bytes.

What this line adds is everything around the model: the build works on the
supported targets (see platform.h), the command line handles stdin, stdout,
verification and long options, damaged and truncated streams are rejected
instead of trusted, and the input timestamps are restored safely.

*/

#include "platform.h"

#include <errno.h>
#include <limits.h>

#include <chrono>

// Threads come from OpenMP, here and inside libsais (built with
// -DLIBSAIS_OPENMP). Without OpenMP everything runs on one thread and -j is
// accepted and ignored.
#if defined(_OPENMP) && !defined(LIBSAIS_OPENMP)
#define LIBSAIS_OPENMP // declares the libsais_*_omp functions
#endif
#include "libsais.h"
#if defined(_OPENMP)
#include <omp.h>
#endif

#include "x86f.h"

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef signed long long S64;

#define BCM_ID 0x214D4342 // "BCM!"  (BCM 1.x; read-only in bcm-ex 2)
#define BCM2_ID 0x324D4342 // "BCM2" (bcm-ex 2 format)

// Container version 2:
//
//   "BCM2"  4 bytes
//   flags   1 byte    bit 0: an x86 BCJ filter was applied to each block
//                     bit 1: LZP was applied to each block
//   method  1 byte    0: BCM's model (bcm 1.x, bcm-ex 2.0)
//                     1: logistic mixing, fast (bcm-ex 2.1, cm1.h)
//                     2: logistic mixing, strong (bcm-ex 2.1, cm2.h)
//   block   4 bytes   block size in bytes, little-endian
//
// Then the same arithmetic-coded stream as 1.x: per block the length, the BWT
// index and the bytes, ending with a zero length and the CRC-32 of the whole
// input. Keeping the block layout identical means the existing model, encoder
// and decoder are reused unchanged; what v2 adds is the filter stage in front
// and a header that says so. The method byte picks the model that codes the
// bytes; the block layout is the same for all of them.

#define BCM2_FLAG_X86 1
#define BCM2_FLAG_LZP 2
#define BCM_EX_VERSION "2.1"

#define BCM_MAX_BLOCK_MB 2047

// Globals

static FILE* in;
static FILE* out;

// The stream header is little-endian and is written byte by byte, so a build on
// any host writes the same four bytes.

static void WriteU32LE(FILE* f, U32 x)
{
    U8 b[4];
    b[0]=U8(x);
    b[1]=U8(x>>8);
    b[2]=U8(x>>16);
    b[3]=U8(x>>24);
    if (fwrite(b, 1, 4, f)!=4)
    {
        perror("Write error");
        exit(1);
    }
}

static U32 ReadU32LE(FILE* f)
{
    U8 b[4];
    if (fread(b, 1, 4, f)!=4)
    {
        fprintf(stderr, "Unexpected end of file\n");
        exit(1);
    }
    return U32(b[0])|(U32(b[1])<<8)|(U32(b[2])<<16)|(U32(b[3])<<24);
}

// Unaligned little-endian load. A plain pointer cast is undefined behaviour in
// C++ and faults on strict-alignment targets; compilers turn this into one load.

static inline U32 LoadU32LE(const U8* p)
{
    return U32(p[0])|(U32(p[1])<<8)|(U32(p[2])<<16)|(U32(p[3])<<24);
}

// The coder's byte I/O. It moves one byte at a time, and getc/putc take the
// stream lock on every call once the process has a second thread (OpenMP's
// pool, with -j) and always on Windows; a buffer of its own avoids both. Only
// the coder reads `in` once the block stream has started (the decoder), and
// only the coder writes `out` (the encoder), so nothing else sees the bytes
// held here. Pending() keeps the progress report exact.

static U8 g_obuf[1<<16];
static size_t g_opos=0;

static void FlushOut()
{
    if (g_opos && fwrite(g_obuf, 1, g_opos, out)!=g_opos)
    {
        perror("Write error");
        exit(1);
    }
    g_opos=0;
}

static inline void PutByte(int c)
{
    g_obuf[g_opos++]=U8(c);
    if (g_opos==sizeof(g_obuf))
        FlushOut();
}

static U8 g_ibuf[1<<16];
static size_t g_ipos=0;
static size_t g_ilen=0;
static int g_eof_reads=0;
static int g_in_bad=0; // 1: read error, 2: input ended long before the stream

// Returns the next byte, or EOF past the end of the input like getc. A valid
// stream is read to its last byte and at most a few bytes past it (see
// DecodeBit); a truncated one, or a damaged length, would otherwise make the
// decoder run on for as many bytes as the block claims, so g_in_bad is set
// and the decoder stops at its next check (CheckIn). This may run on a
// worker thread, so it does not exit by itself.
static int RefillIn()
{
    g_ilen=fread(g_ibuf, 1, sizeof(g_ibuf), in);
    g_ipos=0;
    if (!g_ilen)
    {
        if (ferror(in))
            g_in_bad=1;
        else if (++g_eof_reads>1024)
            g_in_bad=2;
        return EOF;
    }
    return g_ibuf[g_ipos++];
}

static void CheckIn()
{
    if (g_in_bad==1)
    {
        perror("Read error");
        exit(1);
    }
    if (g_in_bad)
    {
        fprintf(stderr, "Unexpected end of file\n");
        exit(1);
    }
}

static inline int GetByte()
{
    return g_ipos<g_ilen? g_ibuf[g_ipos++]: RefillIn();
}

// Position of `in` as the coder sees it, and of `out` including what is still
// in the buffer; -1 if the stream cannot tell (a pipe).
static S64 InPos()
{
    const S64 a=bcm_ftell(in);
    return a<0? a: a-S64(g_ilen-g_ipos);
}

static S64 OutPos()
{
    const S64 b=bcm_ftell(out);
    return b<0? b: b+S64(g_opos);
}

struct Encoder
{
    U32 low;
    U32 high;
    U32 code;

    Encoder()
    {
        low=0;
        high=0xFFFFFFFF;
        code=0;
    }

    void Flush()
    {
        for (int i=0; i<4; ++i)
        {
            PutByte(low>>24);
            low<<=8;
        }
        FlushOut();
    }

    void Init()
    {
        for (int i=0; i<4; ++i)
            code=(code<<8)|GetByte();
    }

    template<int P_LOG>
    void EncodeBit(int bit, U32 p)
    {
        const U32 mid=low+((U64(high-low)*p)>>P_LOG);

        if (bit)
            high=mid;
        else
            low=mid+1;

        // Renormalize
        while ((low^high)<(1<<24))
        {
            PutByte(low>>24);
            low<<=8;
            high=(high<<8)|255;
        }
    }

    template<int P_LOG>
    int DecodeBit(U32 p)
    {
        const U32 mid=low+((U64(high-low)*p)>>P_LOG);

        const int bit=(code<=mid);
        if (bit)
            high=mid;
        else
            low=mid+1;

        // Renormalize. At the very end of a stream this reads past the last
        // byte the encoder wrote; that is harmless and is what bcm 1.65 does,
        // so it must not be turned into an error here.
        while ((low^high)<(1<<24))
        {
            low<<=8;
            high=(high<<8)|255;
            code=(code<<8)|GetByte();
        }

        return bit;
    }
};

template<int RATE>
struct Counter
{
    U16 p;

    Counter()
    {
        p=1<<15; // 0.5
    }

    void Update1()
    {
        p+=(p^0xFFFF)>>RATE;
    }

    void Update0()
    {
        p-=p>>RATE;
    }
};

struct CM: Encoder
{
    Counter<2> counter0[256];
    Counter<4> counter1[256][256];
    Counter<6> counter2[2][256][17];
    int run;
    int c1;
    int c2;

    CM()
    {
        run=0;
        c1=0;
        c2=0;

        for (int i=0; i<2; ++i)
        {
            for (int j=0; j<256; ++j)
            {
                for (int k=0; k<=16; ++k)
                    counter2[i][j][k].p=(k<<12)-(k==16);
            }
        }
    }

    void Put32(U32 x)
    {
        for (U32 i=1U<<31; i>0; i>>=1)
            EncodeBit<1>(x&i, 1); // p=0.5
    }

    U32 Get32()
    {
        U32 x=0;
        for (int i=0; i<32; ++i)
            x+=x+DecodeBit<1>(1); // p=0.5

        return x;
    }

    void Put(int c)
    {
        const int f=(run>2);

        int ctx=1;
        for (int i=128; i>0; i>>=1)
        {
            const int p0=counter0[ctx].p;
            const int p1=counter1[c1][ctx].p;
            const int p2=counter1[c2][ctx].p;
            const int p=(((p0+p1)*7)+p2+p2)>>4;

            // SSE with linear interpolation
            const int j=p>>12;
            const int x1=counter2[f][ctx][j].p;
            const int x2=counter2[f][ctx][j+1].p;
            const int ssep=x1+(((x2-x1)*(p&4095))>>12);

            if (c&i)
            {
                EncodeBit<18>(1, p+ssep+ssep+ssep);

                counter0[ctx].Update1();
                counter1[c1][ctx].Update1();
                counter2[f][ctx][j].Update1();
                counter2[f][ctx][j+1].Update1();

                ctx+=ctx+1;
            }
            else
            {
                EncodeBit<18>(0, p+ssep+ssep+ssep);

                counter0[ctx].Update0();
                counter1[c1][ctx].Update0();
                counter2[f][ctx][j].Update0();
                counter2[f][ctx][j+1].Update0();

                ctx+=ctx;
            }
        }

        c2=c1;
        c1=ctx-256;

        if (c1==c2)
            ++run;
        else
            run=0;
    }

    int Get()
    {
        const int f=(run>2);

        int ctx=1;
        while (ctx<256)
        {
            const int p0=counter0[ctx].p;
            const int p1=counter1[c1][ctx].p;
            const int p2=counter1[c2][ctx].p;
            const int p=(((p0+p1)*7)+p2+p2)>>4;

            // SSE with linear interpolation
            const int j=p>>12;
            const int x1=counter2[f][ctx][j].p;
            const int x2=counter2[f][ctx][j+1].p;
            const int ssep=x1+(((x2-x1)*(p&4095))>>12);

            if (DecodeBit<18>(p+ssep+ssep+ssep))
            {
                counter0[ctx].Update1();
                counter1[c1][ctx].Update1();
                counter2[f][ctx][j].Update1();
                counter2[f][ctx][j+1].Update1();

                ctx+=ctx+1;
            }
            else
            {
                counter0[ctx].Update0();
                counter1[c1][ctx].Update0();
                counter2[f][ctx][j].Update0();
                counter2[f][ctx][j+1].Update0();

                ctx+=ctx;
            }
        }

        c2=c1;
        c1=ctx-256;

        if (c1==c2)
            ++run;
        else
            run=0;

        return c1;
    }
};

struct CRC
{
    U32 tab[8][256];
    U32 crc;

    CRC()
    {
        for (int i=0; i<256; ++i)
        {
            U32 x=i;
            for (int j=0; j<8; ++j)
                x=(x>>1)^(0xEDB88320&-int(x&1));
            tab[0][i]=x;
        }
        for (int i=0; i<256; ++i)
        {
            tab[1][i]=(tab[0][i]>>8)^tab[0][tab[0][i]&255];
            tab[2][i]=(tab[1][i]>>8)^tab[0][tab[1][i]&255];
            tab[3][i]=(tab[2][i]>>8)^tab[0][tab[2][i]&255];
            tab[4][i]=(tab[3][i]>>8)^tab[0][tab[3][i]&255];
            tab[5][i]=(tab[4][i]>>8)^tab[0][tab[4][i]&255];
            tab[6][i]=(tab[5][i]>>8)^tab[0][tab[5][i]&255];
            tab[7][i]=(tab[6][i]>>8)^tab[0][tab[6][i]&255];
        }
        crc=0xFFFFFFFF;
    }

    U32 operator()() const
    {
        return crc^0xFFFFFFFF;
    }

    void Update(const U8* s, int n)
    {
        U32 x=crc;
        while (n>=8)
        {
            const U32 a=LoadU32LE(s);
            const U32 t=LoadU32LE(s+4);
            x^=a;
            x=tab[0][t>>24]
              ^tab[1][(t>>16)&255]
              ^tab[2][(t>>8)&255]
              ^tab[3][t&255]
              ^tab[4][x>>24]
              ^tab[5][(x>>16)&255]
              ^tab[6][(x>>8)&255]
              ^tab[7][x&255];
            s+=8;
            n-=8;
        }
        while (n--)
            x=(x>>8)^tab[0][(x^*s++)&255];
        crc=x;
    }
} crc;

template<typename T>
inline T* MemAlloc(size_t n)
{
    // malloc(0) may return NULL, which is not an error.
    if (!n)
        n=1;

    T* p=reinterpret_cast<T*>(malloc(n*sizeof(T)));
    if (!p)
    {
        perror("Malloc() failed");
        exit(1);
    }
    return p;
}

#include "cm1.h"
#include "cm2.h"

// x86 filter policy: -1 = always off, 0 = decide from the data, 1 = always on.
static int g_x86_mode=0;

// Does this input look like machine code in a way that the filter pays off?
//
// Measured on Silesia: the filter rewrites 9.4% of ooffice (a Windows DLL) and
// 0.24% of mozilla (a tar of mixed files, where it costs 0.4%). Counting the
// bytes the filter would actually rewrite separates the two cleanly, and it is
// one pass over one block.
static bool LooksLikeX86(FILE* f, int bsize)
{
    const S64 pos=bcm_ftell(f);
    if (pos<0)
        return false; // a pipe: do not guess, stay compatible

    U8* buf=MemAlloc<U8>(size_t(bsize));
    const int n=int(fread(buf, 1, size_t(bsize), f));

    // A PE, ELF or Mach-O header is decisive on its own.
    bool looks=false;
    if (n>=4)
    {
        const U8* b=buf;
        if (b[0]=='M' && b[1]=='Z') looks=true;
        if (b[0]==0x7F && b[1]=='E' && b[2]=='L' && b[3]=='F') looks=true;
        if (n>=8)
        {
            const U32 m=LoadU32LE(b);
            if (m==0xFEEDFACE || m==0xFEEDFACF || m==0xCFFAEDFE ||
                m==0xCAFEBABE || m==0xBEBAFECA) looks=true;
        }
    }

    if (!looks)
    {
        U8* copy=MemAlloc<U8>(size_t(n? n: 1));
        memcpy(copy, buf, size_t(n));
        uint32_t st=0;
        st=bcmext::X86Convert(copy, size_t(n), 0, st, true);

        int changed=0;
        for (int i=0; i<n; ++i)
            if (copy[i]!=buf[i])
                ++changed;

        // Above ~1% the filter is rewriting real code and pays off; below it is
        // noise. Verified against the whole Silesia corpus.
        looks = (n>0) && (changed*100 > n);
        free(copy);
    }

    free(buf);

    if (bcm_fseek(f, pos, SEEK_SET))
    {
        perror("Fseek() failed");
        exit(1);
    }
    clearerr(f);
    return looks;
}

// Progress goes to stderr, and only when the size is known: a pipe cannot tell.

static void ReportProgress()
{
    const S64 a=InPos();
    const S64 b=out? OutPos(): 0;
    if (a>=0 && b>=0)
        fprintf(stderr, "%lld -> %lld\r", (long long)a, (long long)b);
}

static int g_test=0; // -t: decompress and verify, write nothing

// Threads (-j#). libsais computes the same forward and inverse BWT with any
// number of threads, and the model always runs on one thread over the blocks
// in order, so the output never depends on the thread count.
//
// With two or more threads, and an input of more than one block, the BWT of
// the next block (or the inverse BWT of the previous one) also runs while the
// model codes the current block, on the other threads. That costs one more
// block buffer.
#define BCM_DEFAULT_THREADS 4
static int g_threads=-1; // -1: not set on the command line, 0: -j0

static int Bwt(U8* buf, int* sa, int n, int threads)
{
#if defined(_OPENMP)
    if (threads>1)
        return libsais_bwt_omp(buf, buf, sa, n, 0, NULL, threads);
#endif
    (void)threads;
    return libsais_bwt(buf, buf, sa, n, 0, NULL);
}

// a has room for n+1 entries. libsais checks idx (1..n) itself; freq must be
// the true byte counts of buf, which the caller counts as it decodes.
static int Unbwt(U8* buf, int* a, int n, const int* freq, int idx, int threads)
{
#if defined(_OPENMP)
    if (threads>1)
        return libsais_unbwt_omp(buf, buf, a, n, freq, idx, threads);
#endif
    (void)threads;
    return libsais_unbwt(buf, buf, a, n, freq, idx);
}

// Is there more input? Works on pipes too.
static bool MoreInput()
{
    const int c=getc(in);
    if (c==EOF)
        return false;
    ungetc(c, in);
    return true;
}

// The model is created after the first block's BWT, and the suffix array is
// released once the last block is transformed: a file of one block never holds
// both the BWT's array and the model's tables.
template<class M>
static void Compress(int bsize, uint32_t filter)
{
    const bool use_x86 = (filter & BCM2_FLAG_X86) != 0;
    uint32_t x86_state = 0;
    S64 flen=-1;

    if (bcm_fseek(in, 0, SEEK_END)==0)
    {
        flen=bcm_ftell(in);
        if (flen<0)
        {
            perror("Ftell() failed");
            exit(1);
        }
        rewind(in);
    }
    else
    {
        // Not seekable (a pipe): keep the configured block size.
        clearerr(in);
    }

    if (flen>=0 && bsize>flen)
        bsize=int(flen);

    U8* buf[2];
    buf[0]=MemAlloc<U8>(size_t(bsize));
    buf[1]=NULL; // the second one only when two blocks overlap
    int* ptr=NULL;
    S64 block_off=0;
    bool more=true;
    int bwt_err=0;

    // Reads the next block into b, filters and transforms it. Returns its
    // length, 0 at the end of the input; *idx is the BWT index. It touches only
    // `in`, the CRC, the filter state and the suffix array, so it can run
    // while the model codes the previous block.
    auto prepare=[&](U8* b, int threads, int* idx) -> int
    {
        const int n=more? int(fread(b, 1, size_t(bsize), in)): 0;
        if (n<=0)
        {
            more=false;
            return 0;
        }

        crc.Update(b, n);

        // The filter runs before the model and is undone after decoding, so
        // the CRC and the block lengths always describe the original bytes.
        if (use_x86)
            x86_state=bcmext::X86Convert(b, size_t(n), uint32_t(block_off), x86_state, true);
        block_off+=n;

        if (!ptr)
            ptr=MemAlloc<int>(size_t(bsize));
        *idx=Bwt(b, ptr, n, threads);
        if (*idx<1)
            bwt_err=1;

        more=MoreInput();
        if (!more)
        {
            free(ptr);
            ptr=NULL;
        }
        return n;
    };

    int cur=0;
    int idx=0;
    int n=prepare(buf[0], g_threads, &idx);

    M* const cm=new M;

    while (n>0 && !bwt_err)
    {
        int idx2=0;
        int n2=0;

        cm->Put32(U32(n)); // Block size
        cm->Put32(U32(idx)); // BWT index

        if (g_threads>1 && more)
        {
            if (!buf[1])
                buf[1]=MemAlloc<U8>(size_t(bsize));
            const U8* const b=buf[cur];
#if defined(_OPENMP)
            #pragma omp parallel sections num_threads(2)
#endif
            {
#if defined(_OPENMP)
                #pragma omp section
#endif
                for (int i=0; i<n; ++i)
                    cm->Put(b[i]);
#if defined(_OPENMP)
                #pragma omp section
#endif
                n2=prepare(buf[cur^1], g_threads-1, &idx2);
            }
            cur^=1;
        }
        else
        {
            for (int i=0; i<n; ++i)
                cm->Put(buf[cur][i]);
            n2=prepare(buf[cur], g_threads, &idx2);
        }

        n=n2;
        idx=idx2;
        ReportProgress();
    }

    if (bwt_err)
    {
        fprintf(stderr, "Libsais_bwt() failed\n");
        exit(1);
    }

    if (ferror(in))
    {
        perror("Read error");
        exit(1);
    }

    cm->Put32(0); // EOF
    cm->Put32(crc()); // CRC32

    cm->Flush();

    free(buf[0]);
    free(buf[1]);
    free(ptr);
}

// max_block: the largest block the stream may hold. A BCM2 header states the
// block size the encoder used, and no block is longer than that.
template<class M>
static void Decompress(uint32_t filter, int max_block)
{
    const bool use_x86 = (filter & BCM2_FLAG_X86) != 0;
    uint32_t x86_state = 0;
    size_t block_off = 0;

    int bsize=0;
    U8* buf[2]={NULL, NULL};
    int freq[2][256];
    int* ptr=NULL;
    int err=0; // set by finish(): 1 corrupt stream, 2 write error

    M* cm=new M;
    cm->Init();

    // A block's header: its length (0 at the end of the stream) and its BWT
    // index. Exits on values no encoder writes.
    auto header=[&](int* idx) -> int
    {
        const int n=int(cm->Get32());
        CheckIn();
        if (n<=0)
            return 0;
        if (!bsize)
        {
            // The first block sets the block size for the whole stream. Refuse
            // a length that asks for more memory than the encoder can have
            // used.
            if (n>max_block)
            {
                fprintf(stderr, "Corrupt input: block size %d is out of range\n", n);
                exit(1);
            }
            bsize=n;
        }

        *idx=int(cm->Get32());
        if (n>bsize || *idx<1 || *idx>n)
        {
            fprintf(stderr, "Corrupt input!\n");
            exit(1);
        }
        return n;
    };

    // The model decodes a block into slot s, counting its bytes. It touches
    // only the model and `in`.
    auto body=[&](int s, int n)
    {
        if (!buf[s])
            buf[s]=MemAlloc<U8>(size_t(bsize));
        U8* const b=buf[s];
        int* const f=freq[s];
        memset(f, 0, sizeof(freq[0]));
        for (int i=0; i<n && !g_in_bad; ++i)
            ++f[b[i]=U8(cm->Get())];
    };

    // Inverse BW-transform of slot s, in place, then the filter, the CRC and
    // the output. It touches only `out`, the CRC, the filter state and the
    // array, so it can run while the model decodes the next block.
    //
    // Any n bytes with an index in 1..n make libsais follow a permutation of
    // 0..n, so a damaged stream cannot make it read or write outside the
    // arrays: it decodes to wrong bytes, which the CRC rejects. (bcm 1.65
    // walked its own array without a check and read ptr[-1].)
    auto finish=[&](int s, int n, int idx, int threads)
    {
        if (!ptr)
            ptr=MemAlloc<int>(size_t(bsize)+1); // libsais needs n+1
        U8* const b=buf[s];
        if (Unbwt(b, ptr, n, freq[s], idx, threads)!=0)
        {
            err=1;
            return;
        }

        if (use_x86)
            x86_state=bcmext::X86Convert(b, size_t(n), uint32_t(block_off), x86_state, false);
        block_off+=size_t(n);

        crc.Update(b, n);

        if (!g_test && fwrite(b, 1, size_t(n), out)!=size_t(n))
            err=2;
    };

    int cur=0;
    int idx=0;
    int n=header(&idx);
    if (n>0)
        body(cur, n);

    U32 stored_crc=0;
    bool have_crc=false;

    while (n>0)
    {
        // The next header is read before this block is inverted, so the last
        // block is never overlapped and needs no second buffer.
        int idx2=0;
        const int n2=header(&idx2);
        if (n2==0)
        {
            // The stream is fully decoded: the model goes before the last
            // inverse BWT allocates its array, so a model that frees its
            // tables in its destructor never coexists with that array.
            stored_crc=cm->Get32();
            have_crc=true;
            CheckIn();
            delete cm;
            cm=NULL;
        }

        if (g_threads>1 && n2>0)
        {
            const int nxt=cur^1;
#if defined(_OPENMP)
            #pragma omp parallel sections num_threads(2)
#endif
            {
#if defined(_OPENMP)
                #pragma omp section
#endif
                body(nxt, n2);
#if defined(_OPENMP)
                #pragma omp section
#endif
                finish(cur, n, idx, g_threads-1);
            }
            cur=nxt;
        }
        else
        {
            finish(cur, n, idx, g_threads);
            if (!err && n2>0)
                body(cur, n2);
        }

        CheckIn();
        if (err==2)
        {
            perror("Fwrite() failed");
            exit(1);
        }
        if (err)
        {
            fprintf(stderr, "Corrupt input!\n");
            exit(1);
        }

        n=n2;
        idx=idx2;
        ReportProgress();
    }

    if (!have_crc)
    {
        stored_crc=cm->Get32(); // an empty stream
        CheckIn();
        delete cm;
    }
    if (stored_crc!=crc())
    {
        fprintf(stderr, "CRC error!\n");
        exit(1);
    }

    free(buf[0]);
    free(buf[1]);
    free(ptr);
}

static void Usage(FILE* f)
{
    fprintf(f,
            "BCM-ex - A BWT-based file compressor, v" BCM_EX_VERSION "\n"
            "Copyright (C) 2008-2021 Ilya Muravyov\n"
            "Extended line (bcm-ex). Writes the BCM2 container, reads BCM 1.x.\n"
            "\n"
            "Usage: bcm [options] infile [outfile]\n"
            "\n"
            "  -b# Block size in MB (default: 16, maximum: 2047)\n"
            "  -d  Decompress\n"
            "  -j# Threads for the BWT (default: 4, -j0: one per processor).\n"
            "      The output is the same with any number of threads\n"
            "  -m# Model: -m1 mixing (default), -m2 stronger mixing (about 2x\n"
            "      slower than -m1, up to ~180 MB more memory), -m0 BCM's\n"
            "      original model (what bcm-ex 2.0 wrote, about 3x faster)\n"
            "  -t  Test the compressed file and exit; writes nothing\n"
            "  -x  x86 filter: -x+ always, -x- never, -x decide from the data\n"
            "      (the default; it pays off on machine code and costs little\n"
            "      elsewhere). Reads BCM! files from 1.x as well as BCM2.\n"
            "  -f  Force overwrite of the output file\n"
            "  -h, --help     Show this help and exit\n"
            "  -V, --version  Show the version and exit\n"
            "\n"
            "Use '-' as infile to read standard input, and as outfile to write\n"
            "standard output. Without an outfile the name is derived from infile\n"
            "('.bcm' is added, or removed when decompressing).\n");
}

int main(int argc, char** argv)
{
    const std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();

    int bsize=1<<24; // 16 MB
    int decompress=0;
    int overwrite=0;
    int method=1;

    while (argc>1 && argv[1][0]=='-' && argv[1][1]!='\0')
    {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
        {
            Usage(stdout);
            return 0;
        }
        if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))
        {
            printf("bcm-ex %s (writes BCM2, reads BCM 1.x, libsais %s)\n",
                   BCM_EX_VERSION, LIBSAIS_VERSION_STRING);
            return 0;
        }

        for (int i=1; argv[1][i]!='\0'; ++i)
        {
            switch (argv[1][i])
            {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                break; // Skip: bcm 1.65 accepted level numbers and ignored them
            case 'b':
            {
                const char* v=&argv[1][i+1];
                char* end=NULL;
                errno=0;
                const long mb=strtol(v, &end, 10);
                if (end==v || errno || mb<1 || mb>BCM_MAX_BLOCK_MB)
                {
                    fprintf(stderr, "Block size is out of range (1..%d MB)\n",
                            BCM_MAX_BLOCK_MB);
                    exit(1);
                }
                bsize=int(mb<<20);
                i=int(end-argv[1])-1; // the digits are consumed here
                break;
            }
            case 'd':
                decompress=1;
                break;
            case 'j':
            {
                // -j# threads, -j0 one per processor
                const char* v=&argv[1][i+1];
                char* end=NULL;
                errno=0;
                const long t=strtol(v, &end, 10);
                if (end==v || errno || t<0 || t>256)
                {
                    fprintf(stderr, "Thread count is out of range (0..256)\n");
                    exit(1);
                }
                g_threads=int(t);
                i=int(end-argv[1])-1; // the digits are consumed here
                break;
            }
            case 'm':
                // -m0: BCM's model (what 2.0 wrote), -m1: mixing (default),
                // -m2: stronger mixing
                if (argv[1][i+1]>='0' && argv[1][i+1]<='2')
                {
                    method=argv[1][i+1]-'0';
                    ++i;
                }
                else
                {
                    fprintf(stderr, "Unknown method: use -m0, -m1 or -m2\n");
                    exit(1);
                }
                break;
            case 't':
                decompress=1;
                g_test=1;
                break;
            case 'f':
                overwrite=1;
                break;
            case 'x':
                // -x+ always filter, -x- never, -x (or nothing) decide from the
                // data. Documented in the usage text.
                if (argv[1][i+1]=='+') { g_x86_mode=1; ++i; }
                else if (argv[1][i+1]=='-') { g_x86_mode=-1; ++i; }
                else g_x86_mode=0;
                break;
            default:
                fprintf(stderr, "Unknown option '-%c'\n", argv[1][i]);
                exit(1);
            }
        }
        --argc;
        ++argv;
    }

#if defined(_OPENMP)
    {
        const int procs=omp_get_num_procs();
        if (g_threads==0)
            g_threads=procs; // -j0
        else if (g_threads<0)
            g_threads=procs<BCM_DEFAULT_THREADS? procs: BCM_DEFAULT_THREADS;
        // libsais' own team inside the two-block overlap
        if (g_threads>2)
            omp_set_max_active_levels(2);
    }
#else
    g_threads=1; // built without OpenMP
#endif

    if (argc<2)
    {
        Usage(stderr);
        exit(1);
    }

    const int in_is_stdin=!strcmp(argv[1], "-");
    const int out_is_stdout=(!g_test && argc>2 && !strcmp(argv[2], "-"));

    if (in_is_stdin)
    {
        // A plain pipe or a terminal; the block size stays as configured.
        bcm_set_binary_stdin();
        in=stdin;
    }
    else
    {
        in=fopen(argv[1], "rb");
        if (!in)
        {
            perror(argv[1]);
            exit(1);
        }
    }

    char ofname[FILENAME_MAX];
    ofname[0]='\0';

    if (out_is_stdout)
        strcpy(ofname, "-");
    else if (g_test)
        ofname[0]='\0'; // no output at all
    else if (argc<3)
    {
        if (in_is_stdin)
        {
            fprintf(stderr, "An output file is required when reading standard input\n");
            exit(1);
        }

        const size_t len=strlen(argv[1]);
        if (decompress)
        {
            if (len>4 && !strcmp(&argv[1][len-4], ".bcm"))
            {
                if (len-4>=sizeof(ofname))
                {
                    fprintf(stderr, "Input file name is too long\n");
                    exit(1);
                }
                memcpy(ofname, argv[1], len-4);
                ofname[len-4]='\0';
            }
            else
            {
                if (len+4>=sizeof(ofname))
                {
                    fprintf(stderr, "Input file name is too long\n");
                    exit(1);
                }
                snprintf(ofname, sizeof(ofname), "%s.out", argv[1]);
            }
        }
        else
        {
            if (len+4>=sizeof(ofname))
            {
                fprintf(stderr, "Input file name is too long\n");
                exit(1);
            }
            snprintf(ofname, sizeof(ofname), "%s.bcm", argv[1]);
        }
    }
    else
    {
        if (strlen(argv[2])>=sizeof(ofname))
        {
            fprintf(stderr, "Output file name is too long\n");
            exit(1);
        }
        strcpy(ofname, argv[2]);
    }

    if (!g_test && !out_is_stdout && !overwrite)
    {
        FILE* f=fopen(ofname, "rb");
        if (f)
        {
            fclose(f);

            fprintf(stderr, "File '%s' already exists. Overwrite (y/n)? ", ofname);
            fflush(stderr);

            if (getchar()!='y')
            {
                fprintf(stderr, "Not overwritten\n");
                exit(1);
            }
        }
    }

    if (decompress)
    {
        const U32 id=ReadU32LE(in);
        uint32_t filter=0;
        int method_in=0;
        int max_block=BCM_MAX_BLOCK_MB<<20;

        if (id==BCM2_ID)
        {
            // bcm-ex 2 container: flags, method, block size.
            const int flags=getc(in);
            const int method=getc(in);
            const U32 blk=ReadU32LE(in);

            if (flags<0 || method<0 || blk==0)
            {
                fprintf(stderr, "%s: Corrupt header\n", argv[1]);
                return 1;
            }
            if (method<0 || method>2)
            {
                fprintf(stderr, "%s: Unsupported method %d\n", argv[1], method);
                return 1;
            }
            if ((flags & ~(BCM2_FLAG_X86|BCM2_FLAG_LZP))!=0)
            {
                fprintf(stderr, "%s: Unsupported flags 0x%x\n", argv[1], flags);
                return 1;
            }
            if (flags & BCM2_FLAG_LZP)
            {
                // The LZP stage measured worse with the BWT on every sample and
                // is not written by this version; refuse rather than guess.
                fprintf(stderr, "%s: LZP streams are not supported\n", argv[1]);
                return 1;
            }
            filter=uint32_t(flags);
            method_in=method;
            if (blk<U32(max_block))
                max_block=int(blk);
        }
        else if (id!=BCM_ID)
        {
            fprintf(stderr, "%s: Not in BCM format\n", argv[1]);
            return 1;
        }
        // id == BCM_ID: BCM 1.65 / bcm-ex 1.x, no filter, no extra header.

        if (!g_test)
        {
            if (out_is_stdout)
            {
                bcm_set_binary_stdout();
                out=stdout;
            }
            else
            {
                out=fopen(ofname, "wb");
            }
            if (!out)
            {
                perror(ofname);
                exit(1);
            }
        }

        fprintf(stderr, "Decompressing '%s':\n", argv[1]);

        if (method_in==2)
            Decompress<CM2>(filter, max_block);
        else if (method_in==1)
            Decompress<CM1>(filter, max_block);
        else
            Decompress<CM>(filter, max_block);
    }
    else
    {
        if (out_is_stdout)
        {
            bcm_set_binary_stdout();
            out=stdout;
        }
        else
        {
            out=fopen(ofname, "wb");
        }
        if (!out)
        {
            perror(ofname);
            exit(1);
        }

        // v2 container. The x86 filter is applied only when it is likely to
        // help: it pays off on machine code and costs a fraction of a percent
        // on everything else, so the decision is made from the data.
        uint32_t filter=0;
        if (g_x86_mode==1)
            filter|=BCM2_FLAG_X86;
        else if (g_x86_mode==0 && LooksLikeX86(in, bsize))
            filter|=BCM2_FLAG_X86;

        WriteU32LE(out, BCM2_ID);
        putc(int(filter&0xFF), out);
        putc(method, out);
        WriteU32LE(out, U32(bsize));

        fprintf(stderr, "Compressing '%s'%s:\n", argv[1],
                (filter&BCM2_FLAG_X86)? " (x86 filter)": "");

        if (method==2)
            Compress<CM2>(bsize, filter);
        else if (method==1)
            Compress<CM1>(bsize, filter);
        else
            Compress<CM>(bsize, filter);
    }

    {
        const S64 a=InPos();
        const S64 b=g_test? 0: OutPos();
        const double sec=std::chrono::duration<double>(
            std::chrono::steady_clock::now()-start).count();
        if (a>=0 && b>=0)
            fprintf(stderr, "%lld -> %lld in %.1f sec\n",
                    (long long)a, (long long)b, sec);
    }

    if (in!=stdin)
        fclose(in);

    if (!g_test && !out_is_stdout)
    {
        if (fclose(out))
        {
            // A full disk is only visible here: report it.
            perror("Fclose() failed");
            exit(1);
        }
    }
    else if (out_is_stdout)
    {
        fflush(stdout);
    }

    // Restore the input file's timestamps on the output, as bcm 1.65 does.
    // A failure here is not worth losing a correctly written file over.
    if (!in_is_stdin && !g_test && !out_is_stdout)
    {
        bcm_stat_t sb;
        if (bcm_stat(argv[1], &sb)==0)
        {
            bcm_utimbuf_t ub;
            ub.actime=sb.st_atime;
            ub.modtime=sb.st_mtime;
            if (bcm_utime(ofname, &ub))
                perror("Utime() failed");
        }
    }

    return 0;
}
