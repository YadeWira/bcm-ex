// cm1.h - bcm-ex method 1: a logistic-mixing model for the BWT output
// ("fast" variant).
//
// Each byte is coded in two steps:
//
//   1. A flag: is this byte the same as the previous one? In BWT output that
//      is true for about two bytes in three, and then nothing else is coded.
//      The flag is predicted by mixing counters for the run length, the
//      previous bytes, the flag history and the length of the last run of the
//      previous byte's value, plus three estimates of how likely the previous
//      byte is according to the byte model's own counters.
//   2. Otherwise the byte is coded bit by bit, knowing it differs from the
//      previous one. Many contexts are taken over the "run-collapsed" history
//      (the previous byte, the byte before its run, and so on), which in BWT
//      output says more than the raw bytes, since runs repeat a byte.
//
//      inputs   order 0 (fast and slow counters, a bit-history state and the
//               last 12 bits seen at the node), order 1, sparse order 1 (the
//               bytes 2, 3 and 4 back), order 2, a hashed order 3, and a run
//               model that bets on the previous byte while the bits so far
//               agree with it.
//      mixing   two mixers with 16-bit weights (selected by bit position and
//               run length; by the previous byte's high nibble and the partial
//               byte), averaged, then refined by two adaptive probability maps
//               (SSE).
//
// Everything is integer arithmetic and table lookups, so every build writes
// the same bytes; the SSE2 code computes exactly what the plain C++ next to it
// computes. Signed right shifts are assumed to be arithmetic (true for GCC,
// Clang and MSVC, and guaranteed from C++20 on).
//
// Memory: about 58 MB (order 2: 36 MB, order 3: 17 MB, SSE: 4 MB).

#ifndef BCM_CM1_H
#define BCM_CM1_H

#include "cm1_tables.h"

#if (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP>=2)) && !defined(CM1_NO_SIMD)
#include <emmintrin.h>
#define CM1_SSE2 1
#define CM1_PREFETCH(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#elif defined(__GNUC__)
#define CM1_PREFETCH(p) __builtin_prefetch(p)
#else
#define CM1_PREFETCH(p) ((void)0)
#endif

#if defined(_MSC_VER)
#define CM1_INLINE __forceinline
#elif defined(__GNUC__)
#define CM1_INLINE inline __attribute__((always_inline))
#else
#define CM1_INLINE inline
#endif

namespace cm1
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
static CM1_INLINE void Update(U16& p, int y)
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
// CM1::st4), so a dot product of up to 16 inputs fits in 32 bits and its value
// >>15 fits in 16 bits.
template<int N>
static inline int Dot(const short* x, const short* w)
{
    int s=0;
    for (int i=0; i<N; ++i)
        s+=x[i]*w[i];
    return s>>15;
}

// |x*f| < 800*7*4096, so the step (x*f+2^14)>>15 is within +-700 and only the
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
#ifdef CM1_SSE2
    __m128i X[NB];
#endif
    short x[N];

    CM1_INLINE void Load()
    {
#ifdef CM1_SSE2
        for (int b=0; b<NB; ++b)
            X[b]=_mm_loadu_si128((const __m128i*)(x+8*b));
#endif
    }

    // i1, i2: the two dot products clamped to -2047..2047, plus 2048.
    CM1_INLINE void Dot(const short* w1, const short* w2, int& i1, int& i2) const
    {
#ifdef CM1_SSE2
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
        i1=Clamp(cm1::Dot<N>(x, w1), -2047, 2047)+2048;
        i2=Clamp(cm1::Dot<N>(x, w2), -2047, 2047)+2048;
#endif
    }

    // w+=(x*f+2^14)>>15, saturated to 16 bits.
    CM1_INLINE void Train(short* w, int f) const
    {
#ifdef CM1_SSE2
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
        cm1::Train<N>(x, w, f);
#endif
    }
};

// Product of the probabilities along the path of byte c in a 256-node tree,
// as a 16-bit probability.
static CM1_INLINE U32 PathP(U32 P, U32 q, int bit)
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

} // namespace cm1

struct CM1: Encoder
{
    cm1::Pool pool;      // owns every table below

    enum
    {
        NIN=16,         // byte model inputs (11 used)
        NF=8,           // flag model inputs
        ROW=272,        // order-2 row: first nibble (16) + 16 second-nibble blocks
        O3BITS=19       // order-3 table: 2^19 blocks of 16 counters
    };

    short* st4;         // [4096] stretch of a 12-bit probability, within +-800
    short* sst;         // [4096] stretch of squash(d), d=-2048..2047 (not clamped)
    cm1::AdaptiveRate<6> rate_run;
    cm1::AdaptiveRate<6> rate_map;
    cm1::HistState hs;

    // byte model
    U16 c0[256];        // order 0, rate 1/4 (learns from every byte)
    U16 c0s[256];       // order 0, rate 1/32 (bytes coded bit by bit)
    U8 hs0[256];        // order 0 bit-history states
    U16 h0[256];        // order 0: the last 12 bits seen at each node, after a leading 1
    U16* c1;            // [b1][ctx]  rate 1/4
    U16* c2;            // [d2][ctx]  (d2: the byte before b1's run)
    U16* c3;            // [b3][ctx]
    U16* c4;            // [d4][ctx]
    U16* o2;            // [b1 d2][ROW]
    U16* o3;            // [2^O3BITS][16], hashed by nibble: b1 d2 d3
    cm1::ACounter* map_h0;     // [2^13][8 bits] h0 -> p
    cm1::ACounter map_s[256*8];    // order 0 states -> p
    cm1::ACounter run_model[64];   // [run length][bit pairs]

    short* w1;          // [8 bits][in run? 0: 16 + run length][NIN]
    short* w2;          // [in run][16 high nibbles of b1][16 partial-byte contexts][NIN]
    U16* a1;            // [run>2][ctx][33]
    U16* a2;            // [b1][ctx][33]

    // flag model
    U16* fc;            // [3][65536] counters
    U16* ffast;         // [b1 b2]
    U16* fr1;           // [last run of b1][run length][flag history]
    short* fw1;         // [32 run lengths][NF]
    short* fw2;         // [256 b1][NF]
    U16* fa;            // [run length][flag history][33]
    U16* fb;            // [b1][run length][33]
    int eh;             // flag history (8 bits)
    int pkey, px1, px2; // cached path estimates (see Flag)

    // history
    int b1, b2, b3, b4, run;
    U64 hd;             // the last 8 bytes that differ from the byte before them
    int d2;             // the byte before b1's run (hd byte 1)
    int lrun[256];      // length of the last finished run of each byte value

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
                t[i*33+j]=U16(cm1::Clamp(CM1_SQUASH[cm1::Clamp(j*128-2048, -2047, 2047)+2048], 16, 65520));
        return t;
    }

    U16* InitRow(size_t n)
    {
        U16* t=pool.Aligned<U16>(n);
        cm1::Fill(t, n, 1<<15);
        return t;
    }

    CM1()
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
            st4[i]=short(cm1::Clamp(st4[i], -800, 800));

        cm1::Fill(c0, 256, 1<<15);
        cm1::Fill(c0s, 256, 1<<15);
        cm1::Fill8(hs0, 256);
        for (int i=0; i<256; ++i)
            h0[i]=1;
        c1=InitRow(4*65536);
        c2=c1+65536;
        c3=c2+65536;
        c4=c3+65536;
        o2=InitRow(size_t(65536)*ROW);
        o3=InitRow(size_t(16)<<O3BITS);
        map_h0=pool.Alloc<cm1::ACounter>(size_t(8)<<13);
        for (int i=0; i<(8<<13); ++i)
            map_h0[i]=cm1::ACounter();

        w1=InitW(8*32, NIN, 3);
        w2=InitW(2*256, NIN, 3);
        a1=InitAPM(2*256);
        a2=InitAPM(65536);

        fc=InitRow(4*65536);
        ffast=fc+3*65536;
        fr1=InitRow(8192);
        fw1=InitW(32, NF, 7);
        fw2=InitW(256, NF, 7);
        fa=InitAPM(1024);
        fb=InitAPM(1024);
        eh=0;
        pkey=-1;
        px1=px2=0;

        b1=b2=b3=b4=run=0;
        hd=0;
        d2=0;
        for (int i=0; i<256; ++i)
            lrun[i]=0;
    }

    // ---- step 1: the "same as the previous byte" flag ----

    template<int DEC>
    CM1_INLINE int Flag(int c)
    {
        const short* const s4=st4;
        const int rr=run>31? 31: run;
        const int h8=eh&255;
        const int q1=cm1::RunBucket(lrun[b1]);
        U16* const k1=fc+(b1<<5|rr);
        U16* const k3=fc+65536+((U32((b1<<16)|(b2<<8)|b3)*0x9E3779B1u)>>16);
        U16* const k5=fc+2*65536+(b1<<8|h8);
        U16* const kf=ffast+(b1<<8|b2);
        U16* const kr1=fr1+(q1<<9|rr<<4|(eh&15));
        cm1::Inputs<NF> M;

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
                    P0=cm1::PathP(P0, c0[n], bb);
                    P1=cm1::PathP(P1, r1[n], bb);
                    P2=cm1::PathP(P2, r2[m], bb);
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
                    P0=cm1::PathP(P0, c0[n], bb);
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
        M.x[7]=s4[*kr1>>4];   // (no bias input: this measured better)
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
        cm1::Update<4>(*k1, y);
        cm1::Update<4>(*k3, y);
        cm1::Update<4>(*k5, y);
        cm1::Update<2>(*kf, y);
        cm1::Update<4>(*kr1, y);
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
        U16* RO2;
        U16* RO3;
        U16* Q3;
        U32 x3;
        short* W1;
        short* W2;
        U16* A1;
        U16* A2;
        cm1::ACounter* RM;
        int B1;
        int cx, nx;
        bool inrun;     // the bits so far agree with b1
    };

    template<int DEC, int BT>
    CM1_INLINE void Bit(ByteState& S, int c)
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
        CM1_PREFETCH(S.A2+cx*33);
        CM1_PREFETCH(S.A2+cx*33+32);
        if (BT==2)
        {
            // order 2: the next nibble's block is one of four adjacent ones;
            // order 3: the same, hashed
            CM1_PREFETCH(S.RO2+16+(cx-4)*64);
            CM1_PREFETCH(S.RO2+16+(cx-4)*64+32);
            S.Q3=o3+(size_t((U32(S.x3|((cx-3)<<24))*0x2545F491u)>>(32-O3BITS+2))<<6);
            CM1_PREFETCH(S.Q3);
            CM1_PREFETCH(S.Q3+32);
        }
        else if (BT==4)
        {
            S.RO2+=16+(cx-16)*16;
            S.RO3=S.Q3+((cx&3)<<4);
            nx=1;
        }

        U16& k0=c0[cx];
        U16& k1=S.R1[cx];
        U16& k2=S.R2[cx];
        U16& k3=S.R3[cx];
        U16& k4=S.R4[cx];
        U16& k5=S.RO2[nx];
        U16& k6=S.RO3[nx];
        U16& k11=c0s[cx];
        U8& t0=hs0[cx];
        cm1::ACounter& m0=map_s[t0<<3|BT];
        cm1::ACounter& mh=map_h0[h0[cx]<<3|BT];
        cm1::Inputs<NIN> M;
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
        M.x[9]=s4[m0.p>>4];
        M.x[10]=s4[mh.p>>4];
        for (int i=11; i<NIN; ++i)
            M.x[i]=0;
        M.Load();

        short* const cw1=S.W1+(BT*32+(inrun? 0: 16))*NIN;
        short* const cw2=S.W2+(((cx>>(BT>3? BT-3: 0))&15)+(inrun? 256: 0))*NIN;
        int i1, i2;
        M.Dot(cw1, cw2, i1, i2);
        const int p1=CM1_SQUASH[i1];
        const int p2=CM1_SQUASH[i2];
        const int i3=((sst[i1]+sst[i2])>>1)+2048;
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
        const int g=(y<<16)+(y<<7)-y-y;
        U16* const ua=ta+(wt>>6);
        U16* const ub=tb+(wt>>6);
        *ua=U16(*ua+((g-*ua)>>7));
        *ub=U16(*ub+((g-*ub)>>7));

        cm1::Update<2>(k0, y);
        cm1::Update<2>(k1, y);
        cm1::Update<4>(k2, y);
        cm1::Update<4>(k3, y);
        cm1::Update<4>(k4, y);
        cm1::Update<4>(k5, y);
        cm1::Update<4>(k6, y);
        cm1::Update<5>(k11, y);
        m0.Update(y, rate_map);
        mh.Update(y, rate_map);
        t0=hs.next[t0][y];
        {
            int h=(h0[cx]<<1)|y;
            if (h>=(2<<12))
                h=(h&4095)|4096;
            h0[cx]=U16(h);
        }
        if (inrun)
        {
            S.RM[BT>>1].Update(y==ex, rate_run);
            inrun=(y==ex);
        }
        cx+=cx+y;
        nx+=nx+y;
    }

    // Byte c was coded: update the histories.
    void EndByte(int c)
    {
        if (c!=b1)
        {
            d2=b1;
            hd=(hd<<8)|U64(c);
        }
        b4=b3;
        b3=b2;
        b2=b1;
        b1=c;
    }

    template<int DEC>
    int Code(int c)
    {
        if (Flag<DEC>(c))
        {
            // the fast order-0 counters learn from every byte
            for (int i=7, n=1; i>=0; --i)
            {
                const int bb=(b1>>i)&1;
                cm1::Update<2>(c0[n], bb);
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
        S.R1=c1+(b1<<8);
        S.R2=c2+(d2<<8);
        S.R3=c3+(b3<<8);
        S.R4=c4+(d4<<8);
        S.RO2=o2+size_t((b1<<8)|d2)*ROW;
        S.x3=U32((b1<<16)|(d2<<8)|d3);
        S.RO3=o3+(size_t((S.x3*0x9E3779B1u)>>(32-O3BITS))<<4);
        S.Q3=S.RO3;
        S.W1=w1+rb*NIN;
        S.W2=w2+(b1>>4)*16*NIN;
        S.A1=a1+(run>2)*256*33;
        S.A2=a2+(b1<<8)*33;
        S.RM=run_model+rb*4;
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
