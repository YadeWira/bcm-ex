// cm2.h - bcm-ex method 2: the "strong" logistic-mixing model for the BWT
// output (slower than method 1 and about 3x its memory, for a better ratio).
//
// Each byte is coded in two steps:
//
//   1. A flag: is this byte the same as the previous one? In BWT output that
//      is true for about two bytes in three, and then nothing else is coded.
//      The flag is predicted by mixing counters for the run length, the
//      previous bytes, the flag history and the lengths of earlier runs, plus
//      three estimates of how likely the previous byte is according to the
//      byte model's own counters.
//   2. Otherwise the byte is coded bit by bit, knowing it differs from the
//      previous one. Many contexts are taken over the "run-collapsed" history
//      (the previous byte, the byte before its run, and so on), which in BWT
//      output says more than the raw bytes, since runs repeat a byte.
//
//      inputs   order 0 (fast and slow counters, a bit-history state and the
//               last 12 bits seen at the node), order 1 (fast, slow and a
//               bit-history state), sparse order 1 (the bytes 2, 3 and 4
//               back), order 2 and its bit-history state, hashed orders 3
//               and 4, two sparse order-2 contexts, a run model that bets on
//               the previous byte while the bits so far agree with it, the
//               same for the byte before the run, and two match models (the
//               byte that last followed the same 4 run-collapsed bytes, or 6
//               raw bytes) that bet on that byte.
//      mixing   four mixers with 16-bit weights (selected by bit position
//               and run length; by the previous byte's high nibble and the
//               partial byte; by the match models' state and the partial
//               byte; by bit position only, learning faster), averaged, then
//               refined by two adaptive probability maps (SSE).
//
// Everything is integer arithmetic and table lookups, so every build writes
// the same bytes; the SSE2 code computes exactly what the plain C++ next to it
// computes. Signed right shifts are assumed to be arithmetic (true for GCC,
// Clang and MSVC, and guaranteed from C++20 on).
//
// Memory: about 182 MB (order 2: 36 MB and its states 18 MB, two sparse order
// 2: 71 MB, orders 3 and 4: 17 MB each, two match tables: 17 MB, SSE: 4 MB).

#ifndef BCM_CM2_H
#define BCM_CM2_H

#include "cm1_tables.h"

#if (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=2)) && !defined(CM1_NO_SIMD)
#include <emmintrin.h>
#define CM2_SSE2 1
#define CM2_PREFETCH(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#elif defined(__GNUC__)
#define CM2_PREFETCH(p) __builtin_prefetch(p)
#else
#define CM2_PREFETCH(p) ((void)0)
#endif

#if defined(_MSC_VER)
#define CM2_INLINE __forceinline
#elif defined(__GNUC__)
#define CM2_INLINE inline __attribute__((always_inline))
#else
#define CM2_INLINE inline
#endif

namespace cm2
{

static inline int Clamp(int x, int lo, int hi)
{
    return x<lo? lo: (x>hi? hi: x);
}

// A probability (16 bits) with an adaptation rate that starts fast and slows
// to 1/2^LIMIT.
template<int LIMIT>
struct AdaptiveRate
{
    enum { N=(1<<(LIMIT+1)) };
    U16 r[N];
    AdaptiveRate()
    {
        for (int i=0; i<N; ++i)
        {
            const int x=131072/(2*i+3);
            r[i]=U16(x<(65536>>LIMIT)? (65536>>LIMIT): x);
        }
    }
};

struct ACounter
{
    U16 p;
    U8 n;
    ACounter(): p(1<<15), n(0) {}

    template<int LIMIT>
    void Update(int y, const AdaptiveRate<LIMIT>& t)
    {
        const U32 r=t.r[n];
        const U32 q=p;
        const U32 up=q+(((65535U-q)*r)>>16);
        const U32 dn=q-((q*r)>>16);
        p=U16(y? up: dn);
        n=U8(n+(n<AdaptiveRate<LIMIT>::N-1));
    }
};

// A probability (16 bits) with a fixed rate 1/2^RATE. It never reaches 0.
template<int RATE>
static CM2_INLINE void Update(U16& p, int y)
{
    const U32 q=p;
    const U32 up=q+((q^0xFFFF)>>RATE);
    const U32 dn=q-(q>>RATE);
    p=U16(y? up: dn);
}

// A bit history in one byte: how many zeros and ones were seen (0..7 each;
// the opposite count is halved on each bit, so the state favours recent
// bits) and the last two bits. A small adaptive table maps it to a
// probability.
struct HistState
{
    U8 next[256][2];
    HistState()
    {
        for (int h=0; h<256; ++h)
        {
            for (int y=0; y<2; ++y)
            {
                int n0=h&7, n1=(h>>3)&7, l=(h>>6)&3;
                if (y)
                {
                    if (n1<7) ++n1;
                    if (n0>1) n0=n0/2+1;
                }
                else
                {
                    if (n0<7) ++n0;
                    if (n1>1) n1=n1/2+1;
                }
                l=((l<<1)|y)&3;
                next[h][y]=U8(n0|(n1<<3)|(l<<6));
            }
        }
    }
};

// Every table a model allocates goes through its Pool, which frees them all
// when the model is destroyed.
struct Pool
{
    enum { MAX=64 };
    void* p[MAX];
    int n;

    Pool(): n(0) {}
    ~Pool()
    {
        while (n>0)
            free(p[--n]);
    }

    template<typename T>
    T* Alloc(size_t count)
    {
        T* t=MemAlloc<T>(count);
        Keep(t);
        return t;
    }

    // 64-byte aligned, for the tables read with SSE2 loads.
    template<typename T>
    T* Aligned(size_t count)
    {
        char* raw=MemAlloc<char>(count*sizeof(T)+64);
        Keep(raw);
        return reinterpret_cast<T*>(raw+((64-(size_t(raw)&63))&63));
    }

private:
    void Keep(void* q)
    {
        if (n==MAX)
        {
            fprintf(stderr, "Too many model tables\n");
            exit(1);
        }
        p[n++]=q;
    }
    Pool(const Pool&);
    Pool& operator=(const Pool&);
};

static inline void Fill(U16* p, size_t n, U16 v)
{
    for (size_t i=0; i<n; ++i)
        p[i]=v;
}

static inline void Fill8(U8* p, size_t n)
{
    for (size_t i=0; i<n; ++i)
        p[i]=0;
}

// Mixer weights are 16-bit (1.0 = 32768) and inputs are within +-800 (see
// CM2::st4), so a dot product of up to 24 inputs fits in 32 bits and its value
// >>15 fits in 16 bits.
template<int N>
static inline int Dot(const short* x, const short* w)
{
    int s=0;
    for (int i=0; i<N; ++i)
        s+=x[i]*w[i];
    return s>>15;
}

// |x*f| <= 800*7*4096, so the step (x*f+2^14)>>15 is within +-700 and only the
// sum needs saturating, as _mm_adds_epi16 does.
template<int N>
static inline void Train(const short* x, short* w, int f)
{
    for (int i=0; i<N; ++i)
        w[i]=short(Clamp(w[i]+((x[i]*f+(1<<14))>>15), -32768, 32767));
}

// N inputs (a multiple of 8) for several mixers.
template<int N>
struct Inputs
{
    enum { NB=N/8 };
#ifdef CM2_SSE2
    __m128i X[NB];
#endif
    short x[N];

    CM2_INLINE void Load()
    {
#ifdef CM2_SSE2
        for (int b=0; b<NB; ++b)
            X[b]=_mm_loadu_si128((const __m128i*)(x+8*b));
#endif
    }

    // i1, i2: the two dot products clamped to -2047..2047, plus 2048.
    CM2_INLINE void Dot(const short* w1, const short* w2, int& i1, int& i2) const
    {
#ifdef CM2_SSE2
        __m128i a=_mm_madd_epi16(X[0], _mm_load_si128((const __m128i*)w1));
        __m128i b=_mm_madd_epi16(X[0], _mm_load_si128((const __m128i*)w2));
        for (int k=1; k<NB; ++k)
        {
            a=_mm_add_epi32(a, _mm_madd_epi16(X[k], _mm_load_si128((const __m128i*)(w1+8*k))));
            b=_mm_add_epi32(b, _mm_madd_epi16(X[k], _mm_load_si128((const __m128i*)(w2+8*k))));
        }
        const __m128i t=_mm_add_epi32(_mm_unpacklo_epi32(a, b), _mm_unpackhi_epi32(a, b));
        const __m128i r=_mm_srai_epi32(_mm_add_epi32(t, _mm_shuffle_epi32(t, 0x4E)), 15);
        // |r| < 2^15, so packing does not saturate
        const __m128i q=_mm_add_epi16(_mm_min_epi16(_mm_max_epi16(_mm_packs_epi32(r, r),
            _mm_set1_epi16(-2047)), _mm_set1_epi16(2047)), _mm_set1_epi16(2048));
        i1=_mm_extract_epi16(q, 0);
        i2=_mm_extract_epi16(q, 1);
#else
        i1=Clamp(cm2::Dot<N>(x, w1), -2047, 2047)+2048;
        i2=Clamp(cm2::Dot<N>(x, w2), -2047, 2047)+2048;
#endif
    }

    // w+=(x*f+2^14)>>15, saturated to 16 bits.
    CM2_INLINE void Train(short* w, int f) const
    {
#ifdef CM2_SSE2
        const __m128i E=_mm_set1_epi16(short(f));
        const __m128i rd=_mm_set1_epi32(1<<14);
        for (int k=0; k<NB; ++k)
        {
            const __m128i l=_mm_mullo_epi16(X[k], E), h=_mm_mulhi_epi16(X[k], E);
            const __m128i g=_mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(_mm_unpacklo_epi16(l, h), rd), 15),
                                            _mm_srai_epi32(_mm_add_epi32(_mm_unpackhi_epi16(l, h), rd), 15));
            _mm_store_si128((__m128i*)(w+8*k), _mm_adds_epi16(_mm_load_si128((const __m128i*)(w+8*k)), g));
        }
#else
        cm2::Train<N>(x, w, f);
#endif
    }
};

// Product of the probabilities along the path of byte c in a 256-node tree,
// as a 16-bit probability.
static CM2_INLINE U32 PathP(U32 P, U32 q, int bit)
{
    return (P*(bit? q: 65536-q))>>16;
}

// Bucket for a run length: 0..11 as they are, then 12..15 for 12-15, 16-31,
// 32-63 and 64 or more.
static inline int RunBucket(int x)
{
    if (x<12)
        return x;
    int l=12;
    for (int t=x>>4; t>0 && l<15; t>>=1)
        ++l;
    return l;
}

} // namespace cm2

struct CM2: Encoder
{
    cm2::Pool pool;      // owns every table below

    enum
    {
        NIN=24,         // byte model inputs (20 used)
        NF=16,          // flag model inputs (10 used)
        ROW=272,        // order-2 row: first nibble (16) + 16 second-nibble blocks
        O3BITS=19,      // order-3 table: 2^19 blocks of 16 counters
        O4BITS=19,      // order-4 table: the same
        MBITS=22        // match tables: 2^22 entries
    };

    short* st4;         // [4096] stretch of a 12-bit probability, within +-800
    short* sst;         // [4096] stretch of squash(d), d=-2048..2047 (not clamped)
    cm2::AdaptiveRate<6> rate_run;
    cm2::AdaptiveRate<6> rate_map;
    cm2::HistState hs;

    // byte model
    U16 c0[256];        // order 0, rate 1/8 (1/4 for repeated bytes)
    U16 c0s[256];       // order 0, rate 1/32
    U8 hs0[256];        // order 0 bit-history states
    U16 h0[256];        // order 0: the last 12 bits seen at each node, after a leading 1
    U16* c1;            // [b1][ctx]  rate 1/4
    U16* c2;            // [d2][ctx]  (d2: the byte before b1's run)
    U16* c3;            // [b3][ctx]
    U16* c4;            // [d4][ctx]
    U16* c5;            // [b1][ctx]  rate 1/64
    U8* hs1;            // [b1][ctx]  bit-history states
    U16* o2;            // [b1 d2][ROW]
    U8* hso2;           // [b1 d2][ROW] bit-history states
    U16* o3;            // [2^O3BITS][16], hashed by nibble: b1 d2 d3
    U16* o4;            // [2^O4BITS][16]: b1 d2 d3 d4
    U16* s14;           // [b1 b4][ROW]
    U16* s56;           // [b5 b6][ROW]
    U8* mt4;            // [2^MBITS][2] match: byte that followed d1..d4, and its count
    U8* mt6;            // [2^MBITS][2] match: byte that followed b1..b6
    cm2::ACounter* map_h0;     // [2^13][8 bits] h0 -> p
    cm2::ACounter map_s[3][256*8]; // states (order 0, order 1, order 2) -> p
    cm2::ACounter run_model[64];   // [run length][bit pairs]
    cm2::ACounter d2_model[128];   // [run length][bit][in run]
    cm2::ACounter m4_model[512];   // [count][bit][in run]
    cm2::ACounter m6_model[512];

    short* w1;          // [8 bits][in run? 0: 16 + run length][NIN]
    short* w2;          // [in run][16 high nibbles of b1][16 partial-byte contexts][NIN]
    short* w3;          // [match states][ctx][NIN]
    short* w4;          // [8 bits][NIN]
    U16* a1;            // [run>2][ctx][33]
    U16* a2;            // [b1][ctx][33]

    // flag model
    U16* fc;            // [3][65536] counters
    U16* ffast;         // [b1 b2]
    U16* fr1;           // [last run of b1][run length][flag history]
    U16* fr2;           // [last run][last run of b1][run length]
    short* fw1;         // [32 run lengths][NF]
    short* fw2;         // [256 b1][NF]
    U16* fa;            // [run length][flag history][33]
    U16* fb;            // [b1][run length][33]
    int eh;             // flag history (8 bits)
    int pkey, px1, px2; // cached path estimates (see Flag)

    // history
    int b1, b2, b3, b4, run;
    U64 hb;             // the last 8 bytes, b1 lowest
    U64 hd;             // the last 8 bytes that differ from the byte before them
    int d2;             // the byte before b1's run (hd byte 1)
    int prl;            // length of the last finished run
    int lrun[256];      // length of the last finished run of each byte value
    U8* me4;            // current match entries
    U8* me6;

    short* InitW(int contexts, int n, int inputs)
    {
        short* w=pool.Aligned<short>(size_t(contexts)*n);
        for (int i=0; i<contexts; ++i)
            for (int k=0; k<n; ++k)
                w[i*n+k]=short(k<inputs? 32768/inputs: 0);
        return w;
    }

    U16* InitAPM(int contexts)
    {
        U16* t=pool.Aligned<U16>(size_t(contexts)*33);
        for (int i=0; i<contexts; ++i)
            for (int j=0; j<33; ++j)
                t[i*33+j]=U16(cm2::Clamp(CM1_SQUASH[cm2::Clamp(j*128-2048, -2047, 2047)+2048], 16, 65520));
        return t;
    }

    U16* InitRow(size_t n)
    {
        U16* t=pool.Aligned<U16>(n);
        cm2::Fill(t, n, 1<<15);
        return t;
    }

    CM2()
    {
        // st4[i]: nearest d with squash(d) closest to the centre of bucket i
        st4=pool.Alloc<short>(4096);
        for (int p=8, j=0; p<65536; p+=16)
        {
            while (j<4095 && CM1_SQUASH[j]<p)
                ++j;
            int d=j;
            if (j>0 && p-CM1_SQUASH[j-1]<CM1_SQUASH[j]-p)
                d=j-1;
            st4[p>>4]=short(d-2048);
        }
        sst=pool.Alloc<short>(4096);
        for (int i=0; i<4096; ++i)
            sst[i]=st4[CM1_SQUASH[i]>>4];
        // Inputs are clamped to +-800 (6.25 in the logit): the fast counters
        // are overconfident at the extremes, and this measured better.
        for (int i=0; i<4096; ++i)
            st4[i]=short(cm2::Clamp(st4[i], -800, 800));

        cm2::Fill(c0, 256, 1<<15);
        cm2::Fill(c0s, 256, 1<<15);
        cm2::Fill8(hs0, 256);
        for (int i=0; i<256; ++i)
            h0[i]=1;
        c1=InitRow(5*65536);
        c2=c1+65536;
        c3=c2+65536;
        c4=c3+65536;
        c5=c4+65536;
        hs1=pool.Alloc<U8>(65536);
        cm2::Fill8(hs1, 65536);
        o2=InitRow(size_t(65536)*ROW);
        hso2=pool.Alloc<U8>(size_t(65536)*ROW);
        cm2::Fill8(hso2, size_t(65536)*ROW);
        o3=InitRow(size_t(16)<<O3BITS);
        o4=InitRow(size_t(16)<<O4BITS);
        s14=InitRow(size_t(65536)*ROW);
        s56=InitRow(size_t(65536)*ROW);
        mt4=pool.Alloc<U8>(size_t(2)<<MBITS);
        mt6=pool.Alloc<U8>(size_t(2)<<MBITS);
        cm2::Fill8(mt4, size_t(2)<<MBITS);
        cm2::Fill8(mt6, size_t(2)<<MBITS);
        map_h0=pool.Alloc<cm2::ACounter>(size_t(8)<<13);
        for (int i=0; i<(8<<13); ++i)
            map_h0[i]=cm2::ACounter();

        w1=InitW(8*32, NIN, 3);
        w2=InitW(2*256, NIN, 3);
        w3=InitW(9*256, NIN, 3);
        w4=InitW(8, NIN, 3);
        a1=InitAPM(2*256);
        a2=InitAPM(65536);

        fc=InitRow(4*65536);
        ffast=fc+3*65536;
        fr1=InitRow(2*8192);
        fr2=fr1+8192;
        fw1=InitW(32, NF, 7);
        fw2=InitW(256, NF, 7);
        fa=InitAPM(1024);
        fb=InitAPM(1024);
        eh=0;
        pkey=-1;
        px1=px2=0;

        b1=b2=b3=b4=run=0;
        hb=hd=0;
        d2=0;
        prl=0;
        for (int i=0; i<256; ++i)
            lrun[i]=0;
        me4=mt4;
        me6=mt6;
    }

    // ---- step 1: the "same as the previous byte" flag ----

    template<int DEC>
    CM2_INLINE int Flag(int c)
    {
        const short* const s4=st4;
        const int rr=run>31? 31: run;
        const int h8=eh&255;
        const int q1=cm2::RunBucket(lrun[b1]);
        U16* const k1=fc+(b1<<5|rr);
        U16* const k3=fc+65536+((U32((b1<<16)|(b2<<8)|b3)*0x9E3779B1u)>>16);
        U16* const k5=fc+2*65536+(b1<<8|h8);
        U16* const kf=ffast+(b1<<8|b2);
        U16* const kr1=fr1+(q1<<9|rr<<4|(eh&15));
        U16* const kr2=fr2+(cm2::RunBucket(prl)<<9|q1<<5|rr);
        cm2::Inputs<NF> M;

        // Estimates of P(next byte == b1) from the byte model's counters: the
        // product of the bit probabilities along b1's path. Within a run of
        // equal bytes the order-1 and order-2 rows do not change, so their
        // estimates are kept.
        {
            const int key=(b1<<8)|b2;
            U32 P0=65536;
            if (key!=pkey)
            {
                pkey=key;
                const U16* const r1=c1+(b1<<8);
                const U16* r2=o2+size_t(key)*ROW;
                U32 P1=65536, P2=65536;
                for (int i=7, n=1, m=1; i>=0; --i)
                {
                    if (i==3)
                    {
                        r2+=16+(n-16)*16;
                        m=1;
                    }
                    const int bb=(b1>>i)&1;
                    P0=cm2::PathP(P0, c0[n], bb);
                    P1=cm2::PathP(P1, r1[n], bb);
                    P2=cm2::PathP(P2, r2[m], bb);
                    n+=n+bb;
                    m+=m+bb;
                }
                px1=s4[(P1>65535? 65535: P1)>>4];
                px2=s4[(P2>65535? 65535: P2)>>4];
            }
            else
            {
                for (int i=7, n=1; i>=0; --i)
                {
                    const int bb=(b1>>i)&1;
                    P0=cm2::PathP(P0, c0[n], bb);
                    n+=n+bb;
                }
            }
            M.x[0]=short(px1);
            M.x[2]=short(px2);
            M.x[4]=s4[(P0>65535? 65535: P0)>>4];
        }
        M.x[1]=s4[*k1>>4];
        M.x[3]=s4[*k3>>4];
        M.x[5]=s4[*k5>>4];
        M.x[6]=s4[*kf>>4];
        M.x[7]=256;
        M.x[8]=s4[*kr1>>4];
        M.x[9]=s4[*kr2>>4];
        for (int i=10; i<NF; ++i)
            M.x[i]=0;
        M.Load();

        short* const cw1=fw1+rr*NF;
        short* const cw2=fw2+b1*NF;
        int i1, i2;
        M.Dot(cw1, cw2, i1, i2);
        const int p1=CM1_SQUASH[i1];
        const int p2=CM1_SQUASH[i2];
        const int i3=((sst[i1]+sst[i2])>>1)+2048;
        const int p3=CM1_SQUASH[i3];
        const int sm=sst[i3]+2048;
        const int lo=sm>>7, wt=sm&127;
        U16* const ta=fa+(rr<<5|(h8&31))*33+lo;
        U16* const tb=fb+(b1<<2|(rr>3? 3: rr))*33+lo;
        const int pa=(ta[0]*(128-wt)+ta[1]*wt)>>7;
        const int pb=(tb[0]*(128-wt)+tb[1]*wt)>>7;
        U32 p=U32((p3*2+pa*3+pb*3)>>3);
        p+=(p==0);  // APM entries can decay to 0; never past 65535

        int y;
        if (DEC)
            y=DecodeBit<16>(p);
        else
        {
            y=(c==b1);
            EncodeBit<16>(y, p);
        }

        M.Train(cw1, ((y<<12)-(p1>>4))*3);
        M.Train(cw2, ((y<<12)-(p2>>4))*3);
        const int g=(y<<16)+(y<<7)-y-y;
        U16* const ua=ta+(wt>>6);
        U16* const ub=tb+(wt>>6);
        *ua=U16(*ua+((g-*ua)>>7));
        *ub=U16(*ub+((g-*ub)>>7));
        cm2::Update<4>(*k1, y);
        cm2::Update<4>(*k3, y);
        cm2::Update<4>(*k5, y);
        cm2::Update<2>(*kf, y);
        cm2::Update<4>(*kr1, y);
        cm2::Update<4>(*kr2, y);
        eh=((eh<<1)|y)&255;
        return y;
    }

    // ---- step 2: a byte that differs from the previous one, bit by bit ----

    struct ByteState
    {
        U16* R1;
        U16* R2;
        U16* R3;
        U16* R4;
        U16* R5;
        U8* H1;
        U16* RO2;
        U16* RO3;
        U16* Q3;
        U16* RO4;
        U16* Q4;
        U16* S14;
        U16* S56;
        U32 x3, x4;
        short* W1;
        short* W2;
        U16* A1;
        U16* A2;
        cm2::ACounter* RM;
        cm2::ACounter* DM;
        int B1, D2;
        int cx, nx;
        bool inrun;     // the bits so far agree with b1
        bool ind2;      // ... with d2
        int mc4, mn4;   // match candidates and their counts (0..15)
        int mc6, mn6;
        bool inm4;      // the bits so far agree with the candidate
        bool inm6;
    };

    template<int DEC, int BT>
    CM2_INLINE void Bit(ByteState& S, int c)
    {
        const short* const s4=st4;
        int& cx=S.cx;
        int& nx=S.nx;
        bool& inrun=S.inrun;

        if (BT==7 && inrun)
        {
            // the first 7 bits are b1's, and the byte is not b1
            cx+=cx+((S.B1&1)^1);
            return;
        }
        CM2_PREFETCH(S.A2+cx*33);
        CM2_PREFETCH(S.A2+cx*33+32);
        if (BT==2)
        {
            // order 2 and the sparse order 2: the next nibble's block is one
            // of four adjacent ones; orders 3 and 4: the same, hashed
            CM2_PREFETCH(S.RO2+16+(cx-4)*64);
            CM2_PREFETCH(S.RO2+16+(cx-4)*64+32);
            CM2_PREFETCH(S.S14+16+(cx-4)*64);
            CM2_PREFETCH(S.S14+16+(cx-4)*64+32);
            CM2_PREFETCH(S.S56+16+(cx-4)*64);
            CM2_PREFETCH(S.S56+16+(cx-4)*64+32);
            S.Q3=o3+(size_t((U32(S.x3|((cx-3)<<24))*0x2545F491u)>>(32-O3BITS+2))<<6);
            CM2_PREFETCH(S.Q3);
            CM2_PREFETCH(S.Q3+32);
            S.Q4=o4+(size_t(((S.x4^(U32(cx-3)<<28))*0x2545F491u)>>(32-O4BITS+2))<<6);
            CM2_PREFETCH(S.Q4);
            CM2_PREFETCH(S.Q4+32);
        }
        else if (BT==4)
        {
            S.RO2+=16+(cx-16)*16;
            S.S14+=16+(cx-16)*16;
            S.S56+=16+(cx-16)*16;
            S.RO3=S.Q3+((cx&3)<<4);
            S.RO4=S.Q4+((cx&3)<<4);
            nx=1;
        }

        U16& k0=c0[cx];
        U16& k1=S.R1[cx];
        U16& k2=S.R2[cx];
        U16& k3=S.R3[cx];
        U16& k4=S.R4[cx];
        U16& k5=S.RO2[nx];
        U16& k6=S.RO3[nx];
        U16& k7=S.RO4[nx];
        U16& k8=S.R5[cx];
        U16& k9=S.S14[nx];
        U16& k10=S.S56[nx];
        U16& k11=c0s[cx];
        U8& t0=hs0[cx];
        U8& t1=S.H1[cx];
        U8& t2=hso2[(S.RO2-o2)+nx];
        cm2::ACounter& m0=map_s[0][t0<<3|BT];
        cm2::ACounter& m1=map_s[1][t1<<3|BT];
        cm2::ACounter& m2=map_s[2][t2<<3|BT];
        cm2::ACounter& mh=map_h0[h0[cx]<<3|BT];
        cm2::Inputs<NIN> M;
        M.x[0]=s4[k0>>4];
        M.x[1]=s4[k4>>4];
        M.x[2]=s4[k2>>4];
        M.x[3]=s4[k5>>4];
        M.x[4]=s4[k6>>4];
        M.x[5]=s4[k1>>4];
        // Run model: while the bits so far agree with b1, bet on its next bit
        // with a confidence learnt per run length.
        int ex=0;
        M.x[6]=0;
        if (inrun)
        {
            ex=(S.B1>>(7-BT))&1;
            const int v=s4[S.RM[BT>>1].p>>4];
            M.x[6]=short(ex? v: -v);
        }
        M.x[7]=s4[k3>>4];
        M.x[8]=s4[k11>>4];
        M.x[9]=s4[k9>>4];
        M.x[10]=s4[k10>>4];
        M.x[11]=s4[k7>>4];
        M.x[12]=s4[k8>>4];
        // Match models: while the bits agree with the candidate, bet on it
        // with a confidence learnt per count, bit and whether b1 still agrees.
        int ex4=0, ex6=0;
        cm2::ACounter* km4=0;
        cm2::ACounter* km6=0;
        M.x[13]=0;
        if (S.inm4)
        {
            ex4=(S.mc4>>(7-BT))&1;
            km4=m4_model+((S.mn4*8+BT)*2+inrun);
            const int v=s4[km4->p>>4];
            M.x[13]=short(ex4? v: -v);
        }
        M.x[14]=0;
        if (S.inm6)
        {
            ex6=(S.mc6>>(7-BT))&1;
            km6=m6_model+((S.mn6*8+BT)*2+inrun);
            const int v=s4[km6->p>>4];
            M.x[14]=short(ex6? v: -v);
        }
        // The same for d2, the byte before b1's run.
        int exd=0;
        cm2::ACounter* kd=0;
        M.x[15]=0;
        if (S.ind2)
        {
            exd=(S.D2>>(7-BT))&1;
            kd=S.DM+(BT*2+inrun);
            const int v=s4[kd->p>>4];
            M.x[15]=short(exd? v: -v);
        }
        M.x[16]=s4[m0.p>>4];
        M.x[17]=s4[m1.p>>4];
        M.x[18]=s4[m2.p>>4];
        M.x[19]=s4[mh.p>>4];
        for (int i=20; i<NIN; ++i)
            M.x[i]=0;
        M.Load();

        const int ms4=S.inm4? 1+ex4: 0;
        const int ms6=S.inm6? 1+ex6: 0;
        short* const cw1=S.W1+(BT*32+(inrun? 0: 16))*NIN;
        short* const cw2=S.W2+(((cx>>(BT>3? BT-3: 0))&15)+(inrun? 256: 0))*NIN;
        short* const cw3=w3+((ms4*3+ms6)*256+cx)*NIN;
        short* const cw4=w4+BT*NIN;
        int i1, i2, i4, i5;
        M.Dot(cw1, cw2, i1, i2);
        M.Dot(cw3, cw4, i4, i5);
        const int p1=CM1_SQUASH[i1];
        const int p2=CM1_SQUASH[i2];
        const int p4=CM1_SQUASH[i4];
        const int p5=CM1_SQUASH[i5];
        const int i3=((sst[i1]+sst[i2]+sst[i4]+sst[i5])>>2)+2048;
        const int p3=CM1_SQUASH[i3];

        const int sm=sst[i3]+2048;
        const int lo=sm>>7, wt=sm&127;
        U16* const ta=S.A1+cx*33+lo;
        U16* const tb=S.A2+cx*33+lo;
        const int pa=(ta[0]*(128-wt)+ta[1]*wt)>>7;
        const int pb=(tb[0]*(128-wt)+tb[1]*wt)>>7;
        U32 p=U32((p3*4+pa*6+pb*6)>>4);
        p+=(p==0);  // APM entries can decay to 0; never past 65535

        int y;
        if (DEC)
            y=DecodeBit<16>(p);
        else
        {
            y=(c>>(7-BT))&1;
            EncodeBit<16>(y, p);
        }

        M.Train(cw1, ((y<<12)-(p1>>4))*3);
        M.Train(cw2, ((y<<12)-(p2>>4))*3);
        M.Train(cw3, ((y<<12)-(p4>>4))*3);
        M.Train(cw4, ((y<<12)-(p5>>4))*7);
        const int g=(y<<16)+(y<<7)-y-y;
        U16* const ua=ta+(wt>>6);
        U16* const ub=tb+(wt>>6);
        *ua=U16(*ua+((g-*ua)>>7));
        *ub=U16(*ub+((g-*ub)>>7));

        cm2::Update<3>(k0, y);
        cm2::Update<2>(k1, y);
        cm2::Update<4>(k2, y);
        cm2::Update<4>(k3, y);
        cm2::Update<4>(k4, y);
        cm2::Update<4>(k5, y);
        cm2::Update<4>(k6, y);
        cm2::Update<4>(k7, y);
        cm2::Update<6>(k8, y);
        cm2::Update<4>(k9, y);
        cm2::Update<4>(k10, y);
        cm2::Update<5>(k11, y);
        m0.Update(y, rate_map);
        m1.Update(y, rate_map);
        m2.Update(y, rate_map);
        mh.Update(y, rate_map);
        t0=hs.next[t0][y];
        t1=hs.next[t1][y];
        t2=hs.next[t2][y];
        {
            int h=(h0[cx]<<1)|y;
            if (h>=(2<<12))
                h=(h&4095)|4096;
            h0[cx]=U16(h);
        }
        if (S.inm4)
        {
            km4->Update(y==ex4, rate_run);
            S.inm4=(y==ex4);
        }
        if (S.inm6)
        {
            km6->Update(y==ex6, rate_run);
            S.inm6=(y==ex6);
        }
        if (S.ind2)
        {
            kd->Update(y==exd, rate_run);
            S.ind2=(y==exd);
        }
        if (inrun)
        {
            S.RM[BT>>1].Update(y==ex, rate_run);
            inrun=(y==ex);
        }
        cx+=cx+y;
        nx+=nx+y;
    }

    // Byte c was coded: update the match entries and the histories.
    void EndByte(int c)
    {
        if (c!=b1)
        {
            // the entries keep the last byte that followed their context
            // and was not b1, with a count of how often it was right
            if (me4[0]==c)
                me4[1]=U8(me4[1]+(me4[1]<255));
            else
            {
                me4[0]=U8(c);
                me4[1]=0;
            }
            if (me6[0]==c)
                me6[1]=U8(me6[1]+(me6[1]<255));
            else
            {
                me6[0]=U8(c);
                me6[1]=0;
            }
            d2=b1;
            hd=(hd<<8)|U64(c);
        }
        hb=(hb<<8)|U64(c);
        b4=b3;
        b3=b2;
        b2=b1;
        b1=c;
        me4=mt4+(size_t((((hd&0xFFFFFFFFull)+1)*0x9E3779B97F4A7C15ull)>>(64-MBITS))<<1);
        me6=mt6+(size_t((((hb&0xFFFFFFFFFFFFull)+3)*0xD6E8FEB86659FD93ull)>>(64-MBITS))<<1);
    }

    template<int DEC>
    int Code(int c)
    {
        if (Flag<DEC>(c))
        {
            // the order-0 counters learn from every byte
            for (int i=7, n=1; i>=0; --i)
            {
                const int bb=(b1>>i)&1;
                cm2::Update<2>(c0[n], bb);
                cm2::Update<5>(c0s[n], bb);
                n+=n+bb;
            }
            run+=(run<65535);   // only min(run, 31) is used
            EndByte(b1);
            return b1;
        }

        pkey=-1;
        ByteState S;
        const int rb=run>15? 15: run;
        const int d3=int(hd>>16)&255, d4=int(hd>>24)&255;
        S.B1=b1;
        S.D2=d2;
        S.R1=c1+(b1<<8);
        S.R2=c2+(d2<<8);
        S.R3=c3+(b3<<8);
        S.R4=c4+(d4<<8);
        S.R5=c5+(b1<<8);
        S.H1=hs1+(b1<<8);
        S.RO2=o2+size_t((b1<<8)|d2)*ROW;
        S.S14=s14+size_t((b1<<8)|b4)*ROW;
        S.S56=s56+size_t(U32(hb>>32)&0xFFFF)*ROW;
        S.x3=U32((b1<<16)|(d2<<8)|d3);
        S.RO3=o3+(size_t((S.x3*0x9E3779B1u)>>(32-O3BITS))<<4);
        S.Q3=S.RO3;
        S.x4=U32((b1<<24)|(d2<<16)|(d3<<8)|d4)*0x9E3779B1u;
        S.x4^=S.x4>>15;
        S.RO4=o4+(size_t((S.x4*0x2C1B3C6Du)>>(32-O4BITS))<<4);
        S.Q4=S.RO4;
        S.W1=w1+rb*NIN;
        S.W2=w2+(b1>>4)*16*NIN;
        S.A1=a1+(run>2)*256*33;
        S.A2=a2+(b1<<8)*33;
        S.RM=run_model+rb*4;
        S.DM=d2_model+(run>7? 7: run)*16;
        S.mc4=me4[0];
        S.mn4=me4[1]>15? 15: me4[1];
        S.mc6=me6[0];
        S.mn6=me6[1]>15? 15: me6[1];
        S.inm4=(S.mc4!=b1);
        S.inm6=(S.mc6!=b1);
        S.ind2=(d2!=b1);
        S.cx=1;
        S.nx=1;
        S.inrun=true;
        Bit<DEC, 0>(S, c);
        Bit<DEC, 1>(S, c);
        Bit<DEC, 2>(S, c);
        Bit<DEC, 3>(S, c);
        Bit<DEC, 4>(S, c);
        Bit<DEC, 5>(S, c);
        Bit<DEC, 6>(S, c);
        Bit<DEC, 7>(S, c);
        prl=run;
        lrun[b1]=run;
        run=0;
        const int nb=S.cx-256;
        EndByte(nb);
        return nb;
    }

    void Put32(U32 v)
    {
        for (U32 i=1U<<31; i>0; i>>=1)
            EncodeBit<1>(v&i, 1);
    }

    U32 Get32()
    {
        U32 v=0;
        for (int i=0; i<32; ++i)
            v+=v+DecodeBit<1>(1);
        return v;
    }

    void Put(int c)
    {
        Code<0>(c);
    }

    int Get()
    {
        return Code<1>(0);
    }
};

#endif
