/* Тест матчера FocalTech: init алгоритма → GetImageTemplate по кадрам → VerifyTwoTemplate.
 * Доказывает, что проприетарный CNN-матчер обрабатывает наши 64x80 кадры и дискриминирует их.
 * Сборка: gcc -O2 -o verify verify.c -ldl
 * Запуск:  LD_PRELOAD=.../focaltech-shim.so ./verify frameA.raw [frameB.raw]
 * Offsets — строго из `nm on your FocalTech .so` (Ghidra добавляет +0x100000!).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define SO "/opt/focaltech/matcher.so"

/* offsets из nm (НЕ из Ghidra) */
#define OFF_ff_algo_do_config    0x43c20
#define OFF_InitFuncLog          0x6e300
#define OFF_GetImageTemplate     0x77da0
#define OFF_VerifyTwoTemplate    0x784a0
#define OFF_GetMatchScores       0x8aee0
#define OFF_InitFuncGetImage     0x6e2a0
#define OFF_InitFuncSpiRead      0x6e2c0
#define OFF_InitFuncSpiWrite     0x6e2b0
#define OFF_InitFuncUsleep       0x6e2d0
#define OFF_InitFuncGetSysTime   0x6e2f0
#define OFF_InitFuncGetMcuStatus 0x6e2e0
#define OFF_InitFuncReadImgInit  0x6e330
#define OFF_InitFuncReadImgStart 0x6e340
#define OFF_InitFuncReadImgEnd   0x6e350
#define OFF_InitFuncWhilePoint   0x6e370
#define OFF_InitFuncGetImageBase 0x6e360

#define W 64
#define H 80

static uint8_t g_frame[W*H];
static int  cb_get_image (uint8_t *buf, uint32_t cnt) { memcpy (buf, g_frame, cnt < W*H ? cnt : W*H); return 0; }
static int  cb_spi_rw    (uint8_t *a, uint8_t *b, uint32_t l) { (void)a;(void)b;(void)l; return 0; }
static void cb_usleep    (uint32_t us) { struct timespec ts={us/1000000,(long)(us%1000000)*1000}; nanosleep(&ts,0); }
static uint32_t cb_systime(void) { static uint32_t t; return t += 10; }
static int  cb_ret0      (void) { return 0; }

static void ftlog (const char *fmt, ...) { va_list ap; va_start(ap,fmt); printf("[ALG] "); vprintf(fmt,ap); printf("\n"); va_end(ap); }

static unsigned long base;
static int (*get_tpl)(uint8_t*, uint32_t*, uint8_t*);

static int load_frame (const char *path) {
  FILE *f = fopen(path,"rb"); if(!f){ fprintf(stderr,"open %s failed\n",path); return -1; }
  size_t n = fread(g_frame,1,W*H,f); fclose(f);
  return n==W*H ? 0 : -1;
}

/* извлечь template текущего g_frame в out (>=131072). Возвращает размер или -1. */
static int extract_tpl (const char *path, uint8_t *out) {
  if (load_frame(path)) return -1;
  uint32_t sz = 0;                 /* IN/OUT: смещение записи; 0 = с начала буфера */
  memset (out, 0, 131072);
  int r = get_tpl (g_frame, &sz, out);
  printf ("  GetImageTemplate(%-14s) ret=%d tplSize=%u\n", path, r, sz);
  return r==0 ? (int)sz : -1;
}

int main (int argc, char **argv)
{
  setvbuf(stdout,NULL,_IONBF,0);
  void *h = dlopen(SO, RTLD_NOW|RTLD_GLOBAL);
  if(!h){ fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
  struct link_map *lm=NULL; dlinfo(h,RTLD_DI_LINKMAP,&lm); base=lm->l_addr;
  printf("base = 0x%lx\n", base);

  void (*init_log)(void*) = (void*)(base+OFF_InitFuncLog);
  int  (*algo_config)(void) = (void*)(base+OFF_ff_algo_do_config);
  get_tpl = (void*)(base+OFF_GetImageTemplate);
  int (*verify2)(uint32_t,uint8_t*,uint32_t,uint8_t*,uint32_t,float*,uint32_t*) = (void*)(base+OFF_VerifyTwoTemplate);
  uint32_t (*get_scores)(void) = (void*)(base+OFF_GetMatchScores);

  #define REG(off,fn) ((void(*)(void*))(base+off))((void*)(fn))
  init_log((void*)ftlog);
  REG(OFF_InitFuncGetImage,     cb_get_image);
  REG(OFF_InitFuncSpiRead,      cb_spi_rw);
  REG(OFF_InitFuncSpiWrite,     cb_spi_rw);
  REG(OFF_InitFuncUsleep,       cb_usleep);
  REG(OFF_InitFuncGetSysTime,   cb_systime);
  REG(OFF_InitFuncGetMcuStatus, cb_ret0);
  REG(OFF_InitFuncReadImgInit,  cb_ret0);
  REG(OFF_InitFuncReadImgStart, cb_ret0);
  REG(OFF_InitFuncReadImgEnd,   cb_ret0);
  REG(OFF_InitFuncWhilePoint,   cb_ret0);
  REG(OFF_InitFuncGetImageBase, cb_ret0);

  /* g_device @0x211400 (фейковая структура — сработала для config) */
  static uint8_t gdev[224];
  *(uint16_t*)(gdev+210)=0x9362; *(int*)(gdev+212)=W; *(int*)(gdev+216)=H;
  *(void**)(base+0x211400)=gdev;

  int r = algo_config();
  printf("ff_algo_do_config = %d\n\n", r);
  if (r!=0){ fprintf(stderr,"config failed\n"); return 2; }

  const char *pA = argc>1 ? argv[1] : "./finger.raw";
  const char *pB = argc>2 ? argv[2] : pA;

  static uint8_t tplA[131072], tplB[131072];
  printf("extract templates:\n");
  int szA = extract_tpl(pA, tplA);
  int szB = extract_tpl(pB, tplB);
  if (szA<0 || szB<0){ fprintf(stderr,"\ntemplate extraction failed (кадр без пальца / низкое качество)\n"); return 3; }

  printf("\nVerifyTwoTemplate(A=%s, B=%s):\n", pA, pB);
  float Hmatrix[10]={0}; uint32_t overlap=0;
  int vr = verify2((uint32_t)szA, tplA, (uint32_t)szB, tplB, /*threshold*/0, Hmatrix, &overlap);
  uint32_t score = get_scores();
  printf("  ret=%d  score=%u  overlapArea=%u\n", vr, score, overlap);
  printf("  Hmatrix = [%.3f %.3f %.3f | %.3f %.3f %.3f]\n",
         Hmatrix[0],Hmatrix[1],Hmatrix[2],Hmatrix[3],Hmatrix[4],Hmatrix[5]);
  return 0;
}
