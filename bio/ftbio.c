/* ftbio — standalone FocalTech FT9348 biometrics: sensor capture + proprietary matcher (dlopen .so).
 * Commands:
 *   ftbio enroll <user>                  — capture finger frames, train, save /etc/focaltech/<user>.tpl
 *   ftbio verify <user>                  — capture a frame, match; exit 0 = match, 1 = no match, 2 = error/timeout
 *   ftbio verify-file <user> <frame.raw> — offline check of one frame (testing)
 *   ftbio check                          — verify the matcher .so is present and compatible
 * Needs root (USB access, reading /etc/focaltech). Build: bio/build.sh
 *
 * Matching uses FocalTech's proprietary libfprint .so (you supply it — see README). We do NOT hardcode
 * offsets: the required focal_* functions and globals are resolved BY NAME from the .so's ELF symbol
 * table at runtime, so any compatible (non-stripped) FocalTech build works. Path: /opt/focaltech/matcher.so
 * (override with env FT_SO).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <syslog.h>
#include <errno.h>

#define SO_DEFAULT "/opt/focaltech/matcher.so"
#define TPLDIR     "/etc/focaltech"
#define VID 0x2808
#define PID 0x9348
#define W 64
#define H 80
#define FRAME (W*H)
#define FID 1

/* ---------- ELF symbol resolver (find focal_* by NAME, not hardcoded offset) ---------- */
static const char *so_path(void){ const char*e=getenv("FT_SO"); return e?e:SO_DEFAULT; }

static uint8_t *elf_map; static size_t elf_len;
static const Elf64_Sym *elf_syms; static size_t elf_nsym; static const char *elf_str;

static int elf_load_symtab(const char *path){
  int fd=open(path,O_RDONLY); if(fd<0){fprintf(stderr,"open %s: %s\n",path,strerror(errno));return -1;}
  struct stat st; if(fstat(fd,&st)){close(fd);return -1;}
  elf_len=st.st_size;
  elf_map=mmap(NULL,elf_len,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
  if(elf_map==MAP_FAILED){elf_map=NULL;return -1;}
  const Elf64_Ehdr *eh=(void*)elf_map;
  if(memcmp(eh->e_ident,ELFMAG,SELFMAG)||eh->e_ident[EI_CLASS]!=ELFCLASS64){fprintf(stderr,"not a 64-bit ELF\n");return -1;}
  const Elf64_Shdr *sh=(void*)(elf_map+eh->e_shoff);
  for(int i=0;i<eh->e_shnum;i++){
    if(sh[i].sh_type==SHT_SYMTAB){
      elf_syms=(void*)(elf_map+sh[i].sh_offset);
      elf_nsym=sh[i].sh_size/sizeof(Elf64_Sym);
      elf_str=(const char*)(elf_map+sh[sh[i].sh_link].sh_offset);
      return 0;
    }
  }
  fprintf(stderr,"%s: no .symtab (stripped) — need a non-stripped FocalTech .so\n",path);
  return -1;
}
/* offset (== nm address) of a symbol by name, or 0 if not found */
static unsigned long sym_off(const char *name){
  if(!elf_syms) return 0;
  for(size_t i=0;i<elf_nsym;i++){
    const char *n=elf_str+elf_syms[i].st_name;
    if(elf_syms[i].st_name && !strcmp(n,name) && elf_syms[i].st_value) return elf_syms[i].st_value;
  }
  return 0;
}

static unsigned long base;
static int missing;
static void *SYM(const char *name){
  unsigned long o=sym_off(name);
  if(!o){ fprintf(stderr,"  MISSING symbol: %s\n",name); missing++; return NULL; }
  return (void*)(base+o);
}

/* ---------- matcher ---------- */
static uint8_t g_frame[FRAME];
static int  cb_get_image(uint8_t*b,uint32_t n){memcpy(b,g_frame,n<FRAME?n:FRAME);return 0;}
static int  cb_spi_rw(uint8_t*a,uint8_t*b,uint32_t l){(void)a;(void)b;(void)l;return 0;}
static void cb_usleep(uint32_t us){struct timespec t={us/1000000,(long)(us%1000000)*1000};nanosleep(&t,0);}
static uint32_t cb_systime(void){static uint32_t t;return t+=10;}
static int  cb_ret0(void){return 0;}
static void ftlog(const char*f,...){if(!getenv("FTLOG"))return;va_list ap;va_start(ap,f);vfprintf(stderr,f,ap);fputc('\n',stderr);va_end(ap);}

static int (*algo_config)(void);
static int (*fn_enroll)(uint16_t,uint16_t,uint8_t*);
static int (*fn_identify)(uint8_t*,uint8_t,uint8_t*,uint8_t*);
static uint32_t (*fn_match_fid)(void);
static uint32_t (*fn_match_score)(void);
static int (*fn_save)(uint16_t,uint8_t*,uint32_t*,uint8_t*);
static int (*fn_init_tpl)(uint16_t,uint8_t,uint32_t,uint8_t*);
static void (*fn_init_log)(void*);
static int (*fn_update)(uint8_t,uint8_t*);

/* register focal_InitFunc* callbacks (stubs; capture is done by us and passed to Enroll/Identify directly) */
static void reg_callbacks(void){
  #define REGN(name,fn) do{ void(*s)(void*)=SYM(name); if(s)s((void*)(fn)); }while(0)
  fn_init_log=SYM("focal_InitFuncLog"); if(fn_init_log) fn_init_log((void*)ftlog);
  REGN("focal_InitFuncGetImage",      cb_get_image);
  REGN("focal_InitFuncSpiRead",       cb_spi_rw);
  REGN("focal_InitFuncSpiWrite",      cb_spi_rw);
  REGN("focal_InitFuncUsleep",        cb_usleep);
  REGN("focal_InitFuncGetSystemTime", cb_systime);
  REGN("focal_InitFuncGetMcuStatus",  cb_ret0);
  REGN("focal_InitFuncReadImageInit", cb_ret0);
  REGN("focal_InitFuncReadImageStart",cb_ret0);
  REGN("focal_InitFuncReadImageEnd",  cb_ret0);
  REGN("focal_InitFuncWhilePoint",    cb_ret0);
  REGN("focal_InitFuncGetImageBase",  cb_ret0);
}

/* open .so, resolve everything by name, configure the algorithm. ret 0 ok, -1 fail */
static int algo_open(void){
  if(elf_load_symtab(so_path())) return -1;
  void*h=dlopen(so_path(),RTLD_LAZY|RTLD_GLOBAL);
  if(!h){fprintf(stderr,"dlopen(%s): %s\n",so_path(),dlerror());return -1;}
  struct link_map*lm=0; dlinfo(h,RTLD_DI_LINKMAP,&lm); base=lm->l_addr;

  missing=0;
  algo_config   =SYM("ff_algo_do_config");
  fn_enroll     =SYM("focal_EnrollByImage");
  fn_identify   =SYM("focal_IdentifyByImage");
  fn_match_fid  =SYM("focal_GetMatchFingerId");
  fn_match_score=SYM("focal_GetMatchScores");
  fn_save       =SYM("focal_SaveAlgTplData");
  fn_init_tpl   =SYM("focal_InitAlgTplData");
  fn_update     =SYM("focal_UpdateTemplate");
  void *g_device=SYM("g_device");
  reg_callbacks();
  if(missing){
    fprintf(stderr,"\n%s is not a compatible FocalTech build (missing symbols above).\n"
                   "Need an FT9366/FT9368-family libfprint .so with a symbol table. See README / get-matcher.sh.\n",so_path());
    return -1;
  }
  /* fake g_device: chip_id@210=0x9362, cols@212, rows@216 */
  static uint8_t gdev[224];
  *(uint16_t*)(gdev+210)=0x9362; *(int*)(gdev+212)=W; *(int*)(gdev+216)=H; *(void**)g_device=gdev;

  int r=algo_config();
  if(r){fprintf(stderr,"ff_algo_do_config=%d\n",r);return -1;}
  if(getenv("FTLOG")){
    if(fn_init_log) fn_init_log((void*)ftlog);
    unsigned long o;
    if((o=sym_off("g_debuginfo")))     *(uint8_t*)(base+o)=2;
    if((o=sym_off("g_lib_log_level"))) *(int*)(base+o)=0;
    if((o=sym_off("g_log_level")))     *(int*)(base+o)=0;
  }
  return 0;
}

/* ---------- sensor ---------- */
static libusb_device_handle*uh; static libusb_context*uctx;
static int wr(unsigned r,unsigned v,unsigned i){return libusb_control_transfer(uh,0x40,r,v,i,NULL,0,800);}
static int finger(void){unsigned char f=0;libusb_control_transfer(uh,0xC0,0x43,0,0,&f,1,800);return f;}
static int grab(unsigned char*buf){wr(0x34,0xff,0);wr(0x34,3,0);wr(0x6f,FRAME,0x9080);int got=0;return libusb_bulk_transfer(uh,0x83,buf,FRAME,&got,2000)?-1:got;}
static double stdev(unsigned char*b,int n){double m=0;for(int i=0;i<n;i++)m+=b[i];m/=n;double s=0;for(int i=0;i<n;i++){double d=b[i]-m;s+=d*d;}return sqrt(s/n);}
static int sensor_open(void){
  libusb_init(&uctx);
  uh=libusb_open_device_with_vid_pid(uctx,VID,PID);
  if(!uh){fprintf(stderr,"device %04x:%04x not found (fprintd stopped? root?)\n",VID,PID);return -1;}
  libusb_set_auto_detach_kernel_driver(uh,1);
  if(libusb_claim_interface(uh,0)){fprintf(stderr,"sensor busy\n");libusb_close(uh);uh=NULL;return -1;}
  wr(0x34,0xff,0);wr(0x34,2,0);wr(0x6f,0,0xff00);
  wr(0x3b,1,0x1f);wr(0x3b,1,0x1e);usleep(10000);
  return 0;
}
static void sensor_close(void){ if(uh){libusb_release_interface(uh,0);libusb_close(uh);} if(uctx)libusb_exit(uctx); }
static int wait_and_grab(unsigned char*buf,int timeout_ms){
  int waited=0;
  for(;;){
    if(finger()){ usleep(60000); if(grab(buf)==FRAME && stdev(buf,FRAME)>=12.0) return 0; }
    usleep(50000); waited+=50; if(timeout_ms>0 && waited>=timeout_ms) return -1;
  }
}
static void wait_release(void){int n=0;while(finger()&&n<200){usleep(50000);n++;}}

/* ---------- template ---------- */
static const char*tpldir(void){const char*e=getenv("FT_TPLDIR");return e?e:TPLDIR;}
static int tpl_path(const char*u,char*o,size_t n){return snprintf(o,n,"%s/%s.tpl",tpldir(),u);}
static int tpl_write(const char*u,uint8_t type,uint32_t size,uint8_t*data){
  /* atomic: tmp + fsync + rename, so a crash or full disk never leaves a broken template */
  mkdir(tpldir(),0700); char p[512]; tpl_path(u,p,sizeof p); char tmp[600]; snprintf(tmp,sizeof tmp,"%s.tmp",p);
  FILE*f=fopen(tmp,"wb"); if(!f){fprintf(stderr,"write %s: %s\n",tmp,strerror(errno));return -1;}
  int ok=fwrite(&type,1,1,f)==1 && fwrite(&size,4,1,f)==1 && fwrite(data,1,size,f)==size;
  if(fflush(f)||fsync(fileno(f)))ok=0;
  if(fclose(f))ok=0;
  chmod(tmp,0600);
  if(!ok||rename(tmp,p)){unlink(tmp);fprintf(stderr,"write %s failed\n",p);return -1;}
  return 0;
}
static int tpl_read(const char*u,uint8_t*type,uint32_t*size,uint8_t*data,uint32_t cap){
  char p[512]; tpl_path(u,p,sizeof p); FILE*f=fopen(p,"rb"); if(!f)return -1;
  if(fread(type,1,1,f)!=1||fread(size,4,1,f)!=1||*size>cap){fclose(f);return -1;}
  size_t rd=fread(data,1,*size,f); fclose(f); return rd==*size?0:-1;
}

/* ---------- commands ---------- */
static uint8_t tpl[8u<<20]; /* 12 frames = 247 KB; grows with enrollment size and self-learning, capped by the matcher at ~0.75 MB */

static int cmd_check(void){
  if(algo_open()){fprintf(stderr,"NOT OK\n");return 2;}
  printf("OK: %s is a compatible FocalTech matcher; symbols resolved, ff_algo_do_config succeeded.\n",so_path());
  return 0;
}
static int cmd_enroll(const char*user){
  const char*en=getenv("FT_ENROLL_N"); const int NEED=(en&&atoi(en)>0)?atoi(en):30;
  if(sensor_open()||algo_open())return 2;
  printf("Enroll '%s': touch the power-button sensor %d times: lift each time, shift and rotate the finger, cover the tip and the edges too.\n",user,NEED);
  int accepted=0;
  for(int c=0;c<NEED*3 && accepted<NEED;c++){
    unsigned char buf[FRAME];
    if(wait_and_grab(buf,30000)){printf("timeout\n");break;}
    memcpy(g_frame,buf,FRAME);
    int er=fn_enroll(FID,(uint16_t)accepted,g_frame);
    if(er==0){accepted++;printf("  [%2d/%d] accepted\n",accepted,NEED);}
    else printf("  frame rejected (er=%d) — touch again, slightly differently\n",er);
    wait_release();
  }
  if(accepted<3){printf("too few frames (%d) — enrollment failed\n",accepted);sensor_close();return 2;}
  uint8_t type=0; uint32_t size=0;
  if(fn_save(FID,&type,&size,tpl)!=0){printf("SaveAlgTplData failed\n");sensor_close();return 2;}
  if(tpl_write(user,type,size,tpl)){sensor_close();return 2;}
  char p[512];tpl_path(user,p,sizeof p); printf("Done: template (%u bytes) -> %s. Frames: %d.\n",size,p,accepted);
  sensor_close(); return 0;
}
/* actMode 2 = verify, as in FocalTech's own driver; with 1 focal_UpdateTemplate refuses (-1) */
static uint8_t actmode(void){const char*e=getenv("FT_ACTMODE");return e?(uint8_t)atoi(e):2;}
static int last_upd; static uint32_t last_score;
static int do_match(void){
  uint8_t fid=0xff,upd=0; int ir=fn_identify(g_frame,actmode(),&fid,&upd);
  uint32_t mfid=fn_match_fid(); uint32_t sc=fn_match_score();
  last_upd=upd; last_score=sc;
  if(getenv("FTLOG"))fprintf(stderr,"identify ret=%d fid=%u score=%u upd=%u\n",ir,mfid,sc,upd);
  return (ir==0 && mfid==FID)?0:1;
}
/* Self-learning. After a match the matcher reports upd=1 but does NOT change the template by itself:
 * focal_UpdateTemplate merges the matched frame in, then we save. Without this the template stays
 * exactly as enrolled, and a 3x4 mm sensor only ever recognises the patches seen at enrollment. */
static int save_updated(const char*user){
  if(!last_upd || !fn_update || getenv("FT_NO_UPDATE")) return 0;
  uint8_t ufid=0xff; int ur=fn_update(1,&ufid);
  if(ur!=0){syslog(LOG_INFO,"template update skipped: UpdateTemplate=%d",ur);return 0;}
  uint8_t type=0; uint32_t size=0;
  int sr=fn_save(FID,&type,&size,tpl);
  if(sr!=0||size==0||size>sizeof tpl){syslog(LOG_WARNING,"template update: SaveAlgTplData=%d size=%u",sr,size);return -1;}
  if(tpl_write(user,type,size,tpl))return -1;
  syslog(LOG_INFO,"template updated for %s: %u bytes",user,size);
  return 1;
}
/* one instance at a time: gdm can start the check twice, and two processes fighting over USB reset the sensor */
static int take_lock(int wait_ms){
  int fd=open("/run/focaltech.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);
  if(fd<0)return 0;
  for(int t=0;;t+=100){ if(!flock(fd,LOCK_EX|LOCK_NB))return 0; if(t>=wait_ms)return -1; usleep(100000); }
}
static int cmd_verify(const char*user){
  uint8_t type=0; uint32_t size=0;
  if(tpl_read(user,&type,&size,tpl,sizeof tpl)){fprintf(stderr,"no template for '%s' (enroll first)\n",user);return 2;}
  if(algo_open())return 2;
  if(fn_init_tpl(FID,type,size,tpl)!=0){fprintf(stderr,"InitAlgTplData failed\n");return 2;}
  if(take_lock(1500)){syslog(LOG_NOTICE,"verify %s: another check is already running",user);return 2;}
  if(sensor_open())return 2;
  fprintf(stderr,"Touch the sensor...\n");
  int timeout_ms=getenv("PAM_TYPE")?8000:15000; const char*to=getenv("FT_TIMEOUT"); if(to){int v=atoi(to);if(v>0)timeout_ms=v;}
  int rc=1,waited=0,touches=0,frames=0,weak=0,wasf=0;
  for(;;){
    unsigned char buf[FRAME];
    int f=finger();
    if(f){
      if(!wasf)touches++;
      usleep(60000);
      if(grab(buf)==FRAME){
        if(stdev(buf,FRAME)>=12.0){frames++; memcpy(g_frame,buf,FRAME); if(do_match()==0){rc=0;break;}}
        else weak++;
      }
    }
    wasf=f;
    usleep(50000); waited+=50; if(waited>=timeout_ms)break;
  }
  int upd = rc==0 ? save_updated(user) : 0;
  sensor_close();
  /* one line per attempt in the journal: `journalctl -t ftbio` */
  syslog(rc==0?LOG_INFO:LOG_NOTICE,"verify %s: %s, touches %d, frames matched %d, weak %d, score %u, template %s, %d ms",
         user, rc==0?"MATCH":(touches?"no match":"no finger"), touches, frames, weak, last_score,
         upd>0?"updated":(upd<0?"UPDATE FAILED":"unchanged"), waited);
  fprintf(stderr, rc==0?"match\n":"no match\n"); return rc;
}
static int cmd_verify_file(const char*user,const char*path){
  uint8_t type=0; uint32_t size=0;
  if(tpl_read(user,&type,&size,tpl,sizeof tpl)){fprintf(stderr,"no template for '%s'\n",user);return 2;}
  if(algo_open())return 2;
  if(fn_init_tpl(FID,type,size,tpl)!=0){fprintf(stderr,"InitAlgTplData failed\n");return 2;}
  FILE*f=fopen(path,"rb"); if(!f){fprintf(stderr,"open %s\n",path);return 2;}
  if(fread(g_frame,1,FRAME,f)!=FRAME){fclose(f);return 2;} fclose(f);
  int rc=do_match();
  if(rc==0 && getenv("FT_UPDATE")) save_updated(user);
  printf("%s: %s score=%u upd=%d\n",path,rc==0?"MATCH":"no match",last_score,last_upd); return rc;
}

int main(int argc,char**argv){
  openlog("ftbio",LOG_PID,LOG_AUTHPRIV);
  const char*pu=getenv("PAM_USER");
  const char*user=(argc>=3)?argv[2]:pu;
  if(argc>=2 && !strcmp(argv[1],"check"))          return cmd_check();
  if(argc>=2 && !strcmp(argv[1],"enroll") && user) return cmd_enroll(user);
  if(argc>=2 && !strcmp(argv[1],"verify") && user) return cmd_verify(user);
  if(argc>=4 && !strcmp(argv[1],"verify-file"))    return cmd_verify_file(argv[2],argv[3]);
  fprintf(stderr,"usage: %s check | enroll <user> | verify <user> | verify-file <user> <frame.raw>\n",argv[0]);
  return 2;
}
