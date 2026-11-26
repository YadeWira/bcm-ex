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

#include "libsais.h"

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef signed long long S64;

#define BCM_ID 0x214D4342 // "BCM!"
#define BCM_EX_VERSION "1.66"

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
            putc(low>>24, out);
            low<<=8;
        }
    }

    void Init()
    {
        for (int i=0; i<4; ++i)
            code=(code<<8)|getc(in);
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
            putc(low>>24, out);
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
            code=(code<<8)|getc(in);
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
} cm;

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

// Progress goes to stderr, and only when the size is known: a pipe cannot tell.

static void ReportProgress()
{
    const S64 a=bcm_ftell(in);
    const S64 b=out? bcm_ftell(out): 0;
    if (a>=0 && b>=0)
        fprintf(stderr, "%lld -> %lld\r", (long long)a, (long long)b);
}

static int g_test=0; // -t: decompress and verify, write nothing

static void Compress(int bsize)
{
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

    U8* buf=MemAlloc<U8>(size_t(bsize));
    int* ptr=MemAlloc<int>(size_t(bsize));

    int n;
    while ((n=fread(buf, 1, size_t(bsize), in))>0)
    {
        crc.Update(buf, n);

        const int idx=libsais_bwt(buf, buf, ptr, n, 0, NULL);
        if (idx<1)
        {
            fprintf(stderr, "Libsais_bwt() failed\n");
            exit(1);
        }

        cm.Put32(U32(n)); // Block size
        cm.Put32(U32(idx)); // BWT index

        for (int i=0; i<n; ++i)
            cm.Put(buf[i]);

        ReportProgress();
    }

    if (ferror(in))
    {
        perror("Read error");
        exit(1);
    }

    cm.Put32(0); // EOF
    cm.Put32(crc()); // CRC32

    cm.Flush();

    free(buf);
    free(ptr);
}

static void Decompress()
{
    int cnt[257];

    int bsize=0;
    U8* buf=NULL;
    int* ptr=NULL;

    cm.Init();

    int n;
    while ((n=int(cm.Get32()))>0)
    {
        if (!bsize)
        {
            // The first block sets the block size for the whole stream. Refuse
            // a header that asks for more memory than any encoder can produce.
            if (n>(BCM_MAX_BLOCK_MB<<20))
            {
                fprintf(stderr, "Corrupt input: block size %d is out of range\n", n);
                exit(1);
            }

            bsize=n;
            buf=MemAlloc<U8>(size_t(bsize));
            ptr=MemAlloc<int>(size_t(bsize));
        }

        const int idx=int(cm.Get32());
        if (n>bsize || idx<1 || idx>n)
        {
            fprintf(stderr, "Corrupt input!\n");
            exit(1);
        }

        // Inverse BW-transform

        memset(cnt, 0, sizeof(cnt));
        for (int i=0; i<n; ++i)
            ++cnt[(buf[i]=U8(cm.Get()))+1];
        for (int i=1; i<256; ++i)
            cnt[i]+=cnt[i-1];

        for (int i=0; i<n; ++i)
            ptr[cnt[buf[i]]++]=i-(i<idx);

        int p=idx-1;
        for (int i=0; i<n; ++i)
        {
            // The walk is driven by data read from the stream. In a valid
            // stream it reaches the -1 entry exactly on the last step; a
            // damaged one can point anywhere, so it is checked before use.
            // bcm 1.65 read ptr[-1] here and crashed on Windows.
            if (p<0 || p>=n)
            {
                fprintf(stderr, "Corrupt input!\n");
                exit(1);
            }

            int c=0;
            int half=127;
            for (int j=0; j<8; ++j)
            {
                if (cnt[c+half]<=p)
                    c+=half+1;
                half>>=1;
            }
            buf[i]=U8(c);
            p=ptr[p];
        }

        crc.Update(buf, n);

        if (!g_test && fwrite(buf, 1, size_t(n), out)!=size_t(n))
        {
            perror("Fwrite() failed");
            exit(1);
        }

        ReportProgress();
    }

    if (cm.Get32()!=crc())
    {
        fprintf(stderr, "CRC error!\n");
        exit(1);
    }

    free(buf);
    free(ptr);
}

static void Usage(FILE* f)
{
    fprintf(f,
            "BCM-ex - A BWT-based file compressor, v" BCM_EX_VERSION "\n"
            "Copyright (C) 2008-2021 Ilya Muravyov\n"
            "Extended line (bcm-ex); stream format identical to BCM 1.65.\n"
            "\n"
            "Usage: bcm [options] infile [outfile]\n"
            "\n"
            "  -b# Block size in MB (default: 16, maximum: 2047)\n"
            "  -d  Decompress\n"
            "  -t  Test the compressed file and exit; writes nothing\n"
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
    const clock_t start=clock();

    int bsize=1<<24; // 16 MB
    int decompress=0;
    int overwrite=0;

    while (argc>1 && argv[1][0]=='-' && argv[1][1]!='\0')
    {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
        {
            Usage(stdout);
            return 0;
        }
        if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))
        {
            printf("bcm-ex %s (stream format BCM 1.65, libsais %s)\n",
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
            case 't':
                decompress=1;
                g_test=1;
                break;
            case 'f':
                overwrite=1;
                break;
            default:
                fprintf(stderr, "Unknown option '-%c'\n", argv[1][i]);
                exit(1);
            }
        }
        --argc;
        ++argv;
    }

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
        if (id!=BCM_ID)
        {
            fprintf(stderr, "%s: Not in BCM format\n", argv[1]);
            return 1;
        }

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

        Decompress();
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

        WriteU32LE(out, BCM_ID);

        fprintf(stderr, "Compressing '%s':\n", argv[1]);

        Compress(bsize);
    }

    {
        const S64 a=bcm_ftell(in);
        const S64 b=g_test? 0: bcm_ftell(out);
        if (a>=0 && b>=0)
            fprintf(stderr, "%lld -> %lld in %.1f sec\n",
                    (long long)a, (long long)b,
                    double(clock()-start)/CLOCKS_PER_SEC);
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
