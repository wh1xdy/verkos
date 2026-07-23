/*
 * vpk — the Verk package manager (VerkOS).
 *
 * A self-contained, source-based (ports/Gentoo-style) package manager written
 * in C. One binary, no shelling out to curl/tar/sha256sum — it links libcurl
 * (fetch), liblzma + zlib (decompress) and implements SHA-256, tar extraction,
 * recipe parsing, dependency resolution and the install database itself. The
 * only child process it spawns is the recipe's own build() (configure/make),
 * which inherently must run real build tools.
 *
 * Commands:
 *   vpk install <pkg>...   resolve deps, fetch, verify, build, install
 *   vpk remove  <pkg>...   remove installed package(s)
 *   vpk list               list installed packages
 *   vpk info    <pkg>      show recipe metadata + install status
 *
 * Layout:
 *   /etc/vpk/recipes/<name>/recipe   recipe (shipped with VerkOS)
 *   /var/lib/vpk/db/<name>/          installed db: files, meta
 *   /var/cache/vpk/                  downloaded source tarballs
 *   /var/tmp/vpk/<name>/             build scratch
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utime.h>
#include <curl/curl.h>
#include <lzma.h>
#include <zlib.h>

/* ---------------------------------------------------------------- config -- */
#define RECIPE_DIR "/etc/vpk/recipes"
#define DB_DIR     "/var/lib/vpk/db"
#define CACHE_DIR  "/var/cache/vpk"
#define BUILD_DIR  "/var/tmp/vpk"
#define VPK_VERSION "0.1"

/* ------------------------------------------------------------- utilities -- */
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("vpk: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}
static void info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("\033[1;36m::\033[0m ", stdout); vfprintf(stdout, fmt, ap); fputc('\n', stdout);
    va_end(ap);
}
static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) die("out of memory"); return p; }
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) die("out of memory"); return p; }

/* mkdir -p */
static int mkpath(const char *path, mode_t mode) {
    char tmp[4096]; size_t len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len && tmp[len-1] == '/') tmp[len-1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0;
            if (mkdir(tmp, mode) && errno != EEXIST) return -1;
            *p = '/'; }
    }
    if (mkdir(tmp, mode) && errno != EEXIST) return -1;
    return 0;
}

/* ---------------------------------------------------------------- sha256 -- */
/* Public-domain-style SHA-256, implemented here so vpk needs no hash tool.   */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;
static uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
static const uint32_t K256[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static void sha256_init(sha256_t *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len=0; s->n=0;
}
static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    for (int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (int i=16;i<64;i++){
        uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for (int i=0;i<64;i++){
        uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^(~e&g);
        t1=h+S1+ch+K256[i]+w[i];
        uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), maj=(a&b)^(a&c)^(b&c);
        t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha256_update(sha256_t *s, const void *data, size_t n) {
    const uint8_t *p=data; s->len+=n;
    while (n) {
        size_t k=64-s->n; if (k>n) k=n;
        memcpy(s->buf+s->n,p,k); s->n+=k; p+=k; n-=k;
        if (s->n==64){ sha256_block(s,s->buf); s->n=0; }
    }
}
static void sha256_final(sha256_t *s, char out[65]) {
    uint64_t bits=s->len*8; uint8_t pad=0x80;
    sha256_update(s,&pad,1);
    uint8_t z=0; while (s->n!=56) sha256_update(s,&z,1);
    uint8_t L[8]; for(int i=0;i<8;i++) L[i]=(bits>>(56-i*8))&0xff;
    sha256_update(s,L,8);
    for (int i=0;i<8;i++) sprintf(out+i*8,"%08x",s->h[i]);
    out[64]=0;
}
static int sha256_file(const char *path, char out[65]) {
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    sha256_t s; sha256_init(&s); uint8_t b[65536]; size_t r;
    while ((r=fread(b,1,sizeof b,f))>0) sha256_update(&s,b,r);
    fclose(f); sha256_final(&s,out); return 0;
}

/* ------------------------------------------------------------- download --- */
static size_t wr_cb(void *p, size_t sz, size_t nm, void *fp) {
    return fwrite(p, sz, nm, (FILE*)fp);
}
static int download(const char *url, const char *dest) {
    FILE *f=fopen(dest,"wb"); if(!f) return -1;
    CURL *c=curl_easy_init(); if(!c){ fclose(f); return -1; }
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wr_cb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,f);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"vpk/" VPK_VERSION);
    CURLcode rc=curl_easy_perform(c);
    curl_easy_cleanup(c); fclose(f);
    if (rc!=CURLE_OK){ fprintf(stderr,"vpk: download failed: %s\n",curl_easy_strerror(rc)); unlink(dest); return -1; }
    return 0;
}

/* ----------------------------------------------------------- decompress --- */
/* Decompress a .tar.xz / .tar.gz into a plain .tar file (self-contained). */
static int decompress_xz(const char *in, const char *out) {
    FILE *fi=fopen(in,"rb"), *fo=fopen(out,"wb");
    if(!fi||!fo){ if(fi)fclose(fi); if(fo)fclose(fo); return -1; }
    lzma_stream strm=LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&strm,UINT64_MAX,LZMA_CONCATENATED)!=LZMA_OK){ fclose(fi);fclose(fo);return -1; }
    uint8_t ib[65536], ob[65536]; lzma_action act=LZMA_RUN; int ret=0;
    strm.next_in=NULL; strm.avail_in=0; strm.next_out=ob; strm.avail_out=sizeof ob;
    for (;;) {
        if (strm.avail_in==0 && !feof(fi)) {
            strm.next_in=ib; strm.avail_in=fread(ib,1,sizeof ib,fi);
            if (feof(fi)) act=LZMA_FINISH;
        }
        lzma_ret r=lzma_code(&strm,act);
        if (strm.avail_out==0 || r==LZMA_STREAM_END) {
            size_t n=sizeof ob - strm.avail_out;
            if (fwrite(ob,1,n,fo)!=n){ ret=-1; break; }
            strm.next_out=ob; strm.avail_out=sizeof ob;
        }
        if (r==LZMA_STREAM_END) break;
        if (r!=LZMA_OK){ ret=-1; break; }
    }
    lzma_end(&strm); fclose(fi); fclose(fo);
    return ret;
}
static int decompress_gz(const char *in, const char *out) {
    gzFile gi=gzopen(in,"rb"); FILE *fo=fopen(out,"wb");
    if(!gi||!fo){ if(gi)gzclose(gi); if(fo)fclose(fo); return -1; }
    char b[65536]; int n, ret=0;
    while ((n=gzread(gi,b,sizeof b))>0) if (fwrite(b,1,n,fo)!=(size_t)n){ ret=-1; break; }
    if (n<0) ret=-1;
    gzclose(gi); fclose(fo); return ret;
}
static int decompress_to_tar(const char *in, const char *out) {
    size_t l=strlen(in);
    if (l>3 && !strcmp(in+l-3,".xz"))  return decompress_xz(in,out);
    if (l>3 && !strcmp(in+l-3,".gz"))  return decompress_gz(in,out);
    if (l>4 && !strcmp(in+l-4,".tgz")) return decompress_gz(in,out);
    return -1;
}

/* ---------------------------------------------------------------- untar --- */
/* Minimal ustar extractor: regular files, directories, symlinks. Extracts
 * into `dest`, stripping the leading path component (like tar --strip 1). */
static long octal(const char *p, int n) { long v=0; for(int i=0;i<n && p[i];i++){ if(p[i]<'0'||p[i]>'7')break; v=v*8+(p[i]-'0'); } return v; }
static int untar(const char *tarfile, const char *dest, int strip) {
    FILE *f=fopen(tarfile,"rb"); if(!f) return -1;
    uint8_t hdr[512]; int ret=0;
    for (;;) {
        if (fread(hdr,1,512,f)!=512) break;
        int empty=1; for(int i=0;i<512;i++) if(hdr[i]){empty=0;break;}
        if (empty) break;                       /* end of archive */
        char name[101]; memcpy(name,hdr,100); name[100]=0;
        char linkname[101]; memcpy(linkname,hdr+157,100); linkname[100]=0;
        long size=octal((char*)hdr+124,12);
        long mode=octal((char*)hdr+100,8);
        long mtime=octal((char*)hdr+136,12);   /* preserve mtime — else make
                                                  thinks autotools files are
                                                  stale and re-runs aclocal. */
        char type=hdr[156];
        /* ustar prefix (long paths) */
        char full[256]; if (hdr[345]) { char pre[156]; memcpy(pre,hdr+345,155); pre[155]=0;
            snprintf(full,sizeof full,"%s/%s",pre,name); } else snprintf(full,sizeof full,"%s",name);
        /* strip leading components */
        char *rel=full; for (int s=0;s<strip;s++){ char *sl=strchr(rel,'/'); if(!sl){rel=NULL;break;} rel=sl+1; }
        long blocks=(size+511)/512;
        if (!rel || !*rel) {                    /* skip data of stripped entry */
            for (long i=0;i<blocks;i++) if(fread(hdr,1,512,f)!=512){ret=-1;goto out;}
            continue;
        }
        char path[4096]; snprintf(path,sizeof path,"%s/%s",dest,rel);
        if (type=='5') {                        /* directory */
            mkpath(path,0755);
        } else if (type=='2') {                 /* symlink */
            char *sl=strrchr(path,'/'); if(sl){*sl=0; mkpath(path,0755); *sl='/';}
            unlink(path); if (symlink(linkname,path)) { /* ignore */ }
        } else if (type=='0' || type=='\0') {   /* regular file */
            char *sl=strrchr(path,'/'); if(sl){*sl=0; mkpath(path,0755); *sl='/';}
            FILE *o=fopen(path,"wb");
            if(!o){ ret=-1; goto out; }
            long left=size; uint8_t b[512];
            for (long i=0;i<blocks;i++){
                if(fread(b,1,512,f)!=512){ ret=-1; fclose(o); goto out; }
                long w=left<512?left:512; if(w>0) fwrite(b,1,w,o); left-=w;
            }
            fclose(o); chmod(path,mode?mode:0644);
            if (mtime){ struct utimbuf ut={mtime,mtime}; utime(path,&ut); }
        } else {                                /* skip other types' data */
            for (long i=0;i<blocks;i++) if(fread(hdr,1,512,f)!=512){ret=-1;goto out;}
        }
    }
out:
    fclose(f); return ret;
}

/* --------------------------------------------------------------- recipe --- */
typedef struct {
    char name[64], version[64], source[1024], sha256[65];
    char depends[1024];   /* space-separated */
    char build[8192];     /* body of build() */
} recipe_t;

static int recipe_path(const char *name, char *out, size_t n) {
    snprintf(out,n,"%s/%s/recipe",RECIPE_DIR,name); return access(out,R_OK);
}
/* very small parser: key=value lines + a build() { ... } block */
static int recipe_load(const char *name, recipe_t *r) {
    char path[512]; if (recipe_path(name,path,sizeof path)) return -1;
    FILE *f=fopen(path,"r"); if(!f) return -1;
    memset(r,0,sizeof *r); snprintf(r->name,sizeof r->name,"%s",name);
    char line[4096]; int inbuild=0; size_t bl=0;
    while (fgets(line,sizeof line,f)) {
        if (inbuild) {
            if (line[0]=='}' ) { inbuild=0; continue; }
            size_t ll=strlen(line);
            if (bl+ll < sizeof r->build){ memcpy(r->build+bl,line,ll); bl+=ll; r->build[bl]=0; }
            continue;
        }
        if (strstr(line,"build()")){ inbuild=1; continue; }
        char *eq=strchr(line,'='); if(!eq) continue;
        *eq=0; char *k=line, *v=eq+1;
        /* trim newline + surrounding quotes on value */
        size_t vl=strlen(v); while(vl && (v[vl-1]=='\n'||v[vl-1]=='\r')) v[--vl]=0;
        if (vl>=2 && ((v[0]=='"'&&v[vl-1]=='"')||(v[0]=='\''&&v[vl-1]=='\''))){ v[vl-1]=0; v++; }
        if      (!strcmp(k,"name"))    snprintf(r->name,sizeof r->name,"%s",v);
        else if (!strcmp(k,"version")) snprintf(r->version,sizeof r->version,"%s",v);
        else if (!strcmp(k,"source"))  snprintf(r->source,sizeof r->source,"%s",v);
        else if (!strcmp(k,"sha256"))  snprintf(r->sha256,sizeof r->sha256,"%s",v);
        else if (!strcmp(k,"depends")) snprintf(r->depends,sizeof r->depends,"%s",v);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------- db --- */
static int is_installed(const char *name) {
    char p[512]; snprintf(p,sizeof p,"%s/%s",DB_DIR,name); struct stat st;
    return stat(p,&st)==0;
}
static void db_record(const recipe_t *r, const char *destdir);  /* fwd */

/* ------------------------------------------------------- dep resolution --- */
/* Depth-first topological order of deps (installs deps before dependents).   */
#define MAXQ 256
static void resolve(const char *name, char order[][64], int *n, char seen[][64], int *sn) {
    for (int i=0;i<*sn;i++) if(!strcmp(seen[i],name)) return;      /* already visited */
    if (*sn>=MAXQ) die("dependency graph too large");
    snprintf(seen[(*sn)++],64,"%s",name);
    recipe_t r;
    if (recipe_load(name,&r)) die("no recipe for '%s'",name);
    /* visit deps first */
    char deps[1024]; snprintf(deps,sizeof deps,"%s",r.depends);
    for (char *tok=strtok(deps," "); tok; tok=strtok(NULL," ")) {
        if (!*tok) continue;
        resolve(tok,order,n,seen,sn);
    }
    if (*n>=MAXQ) die("too many packages");
    snprintf(order[(*n)++],64,"%s",name);                          /* post-order */
}

/* --------------------------------------------------------------- build ---- */
static int run_build(const recipe_t *r, const char *srcdir, const char *destdir) {
    /* write the recipe build body to a script and run it with DESTDIR set. */
    char script[512]; snprintf(script,sizeof script,"%s/%s/build.sh",BUILD_DIR,r->name);
    FILE *s=fopen(script,"w"); if(!s) return -1;
    fprintf(s,"#!/bin/sh\nset -e\nDESTDIR=\"%s\"\nexport DESTDIR\n%s\n",destdir,r->build);
    fclose(s); chmod(script,0755);
    pid_t pid=fork();
    if (pid==0){
        if (chdir(srcdir)!=0) _exit(127);
        execl("/bin/sh","sh",script,(char*)NULL); _exit(127);
    }
    int st; waitpid(pid,&st,0);
    return (WIFEXITED(st)&&WEXITSTATUS(st)==0)?0:-1;
}

/* -------------------------------------------------- install one package --- */
/* copy DESTDIR tree into / and record relative paths into the db file list. */
static void copy_and_record(const char *base, const char *rel, FILE *db) {
    char src[4096]; snprintf(src,sizeof src,"%s%s%s",base,*rel?"/":"",rel);
    DIR *d=opendir(src); if(!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char cr[4096]; snprintf(cr,sizeof cr,"%s%s%s",rel,*rel?"/":"",e->d_name);
        char sp[4096]; snprintf(sp,sizeof sp,"%s/%s",base,cr);
        char dp[4096]; snprintf(dp,sizeof dp,"/%s",cr);
        struct stat st; if (lstat(sp,&st)) continue;
        if (S_ISDIR(st.st_mode)) {
            mkpath(dp,0755);
            fprintf(db,"d %s\n",cr);
            copy_and_record(base,cr,db);
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[4096]; ssize_t k=readlink(sp,tgt,sizeof tgt-1); if(k<0)continue; tgt[k]=0;
            unlink(dp); if(symlink(tgt,dp)){}
            fprintf(db,"f %s\n",cr);
        } else if (S_ISREG(st.st_mode)) {
            int in=open(sp,O_RDONLY); if(in<0)continue;
            int out=open(dp,O_WRONLY|O_CREAT|O_TRUNC,st.st_mode&07777);
            if(out<0){ close(in); continue; }
            char b[65536]; ssize_t rn;
            while ((rn=read(in,b,sizeof b))>0) if(write(out,b,rn)!=rn) break;
            close(in); close(out);
            fprintf(db,"f %s\n",cr);
        }
    }
    closedir(d);
}

static void install_one(const recipe_t *r) {
    info("installing %s %s", r->name, r->version);
    mkpath(CACHE_DIR,0755);
    char work[512]; snprintf(work,sizeof work,"%s/%s",BUILD_DIR,r->name);
    mkpath(work,0755);

    /* 1. fetch source (cached by basename) */
    const char *bn=strrchr(r->source,'/'); bn=bn?bn+1:r->source;
    char tarball[1024]; snprintf(tarball,sizeof tarball,"%s/%s",CACHE_DIR,bn);
    if (access(tarball,R_OK)) { info("fetch %s",r->source);
        if (download(r->source,tarball)) die("download failed"); }

    /* 2. verify sha256 */
    if (r->sha256[0]) {
        char got[65]; if (sha256_file(tarball,got)) die("hash failed");
        if (strcmp(got,r->sha256)) die("sha256 mismatch for %s\n  want %s\n  got  %s",bn,r->sha256,got);
        info("verified sha256");
    }

    /* 3. decompress + extract (strip leading dir) */
    char tar[1024]; snprintf(tar,sizeof tar,"%s/src.tar",work);
    if (decompress_to_tar(tarball,tar)) die("decompress failed");
    char srcdir[512]; snprintf(srcdir,sizeof srcdir,"%s/src",work);
    mkpath(srcdir,0755);
    if (untar(tar,srcdir,1)) die("extract failed");
    unlink(tar);

    /* 4. build into DESTDIR */
    char dest[512]; snprintf(dest,sizeof dest,"%s/dest",work);
    char rm[600]; snprintf(rm,sizeof rm,"%s",dest);
    /* fresh dest */
    { char cmd[700]; snprintf(cmd,sizeof cmd,"%s",dest); }
    mkpath(dest,0755);
    info("building %s", r->name);
    if (run_build(r,srcdir,dest)) die("build failed for %s",r->name);

    /* 5. merge into / and record db */
    char dbdir[512]; snprintf(dbdir,sizeof dbdir,"%s/%s",DB_DIR,r->name);
    mkpath(dbdir,0755);
    char files[600]; snprintf(files,sizeof files,"%s/files",dbdir);
    FILE *db=fopen(files,"w"); if(!db) die("cannot write db");
    copy_and_record(dest,"",db);
    fclose(db);
    char meta[600]; snprintf(meta,sizeof meta,"%s/meta",dbdir);
    FILE *m=fopen(meta,"w");
    if(m){ fprintf(m,"name=%s\nversion=%s\n",r->name,r->version); fclose(m); }
    info("installed %s %s", r->name, r->version);
    (void)rm;
}

/* ----------------------------------------------------------- commands ----- */
static void cmd_install(int argc, char **argv) {
    char order[MAXQ][64], seen[MAXQ][64]; int n=0, sn=0;
    for (int i=0;i<argc;i++) resolve(argv[i],order,&n,seen,&sn);
    info("resolved %d package(s) to install", n);
    for (int i=0;i<n;i++) {
        if (is_installed(order[i])) { info("%s already installed, skipping",order[i]); continue; }
        recipe_t r; if (recipe_load(order[i],&r)) die("no recipe for '%s'",order[i]);
        install_one(&r);
    }
}
static void cmd_remove(int argc, char **argv) {
    for (int i=0;i<argc;i++) {
        if (!is_installed(argv[i])) { fprintf(stderr,"vpk: %s not installed\n",argv[i]); continue; }
        char files[512]; snprintf(files,sizeof files,"%s/%s/files",DB_DIR,argv[i]);
        FILE *f=fopen(files,"r");
        if (f) {
            /* collect then delete files first, dirs after (reverse) */
            char line[4096]; char dirs[4096][256]; int nd=0;
            while (fgets(line,sizeof line,f)) {
                char t=line[0]; char *rel=line+2; size_t l=strlen(rel);
                while(l&&(rel[l-1]=='\n'||rel[l-1]=='\r'))rel[--l]=0;
                char p[4096]; snprintf(p,sizeof p,"/%s",rel);
                if (t=='f') unlink(p);
                else if (t=='d' && nd<4096) snprintf(dirs[nd++],256,"%s",p);
            }
            for (int k=nd-1;k>=0;k--) rmdir(dirs[k]);   /* rmdir empties, deepest first */
            fclose(f);
        }
        char dbdir[512]; snprintf(dbdir,sizeof dbdir,"%s/%s",DB_DIR,argv[i]);
        char cmd[600]; snprintf(cmd,sizeof cmd,"%s/files",dbdir); unlink(cmd);
        snprintf(cmd,sizeof cmd,"%s/meta",dbdir); unlink(cmd);
        rmdir(dbdir);
        info("removed %s", argv[i]);
    }
}
static void cmd_list(void) {
    DIR *d=opendir(DB_DIR); if(!d){ return; }
    struct dirent *e;
    while ((e=readdir(d))) {
        if (e->d_name[0]=='.') continue;
        char meta[512]; snprintf(meta,sizeof meta,"%s/%s/meta",DB_DIR,e->d_name);
        char ver[64]="?"; FILE *m=fopen(meta,"r");
        if(m){ char l[128]; while(fgets(l,sizeof l,m)) if(!strncmp(l,"version=",8)){ snprintf(ver,sizeof ver,"%s",l+8); ver[strcspn(ver,"\n")]=0; } fclose(m); }
        printf("%-20s %s\n", e->d_name, ver);
    }
    closedir(d);
}
static void cmd_info(const char *name) {
    recipe_t r; if (recipe_load(name,&r)) die("no recipe for '%s'",name);
    printf("Name:      %s\n",r.name);
    printf("Version:   %s\n",r.version);
    printf("Source:    %s\n",r.source);
    printf("Depends:   %s\n",r.depends[0]?r.depends:"(none)");
    printf("Installed: %s\n",is_installed(name)?"yes":"no");
}

static void usage(void) {
    printf("vpk %s — the Verk package manager\n\n"
           "usage:\n"
           "  vpk install <pkg>...   build+install from source (with deps)\n"
           "  vpk remove  <pkg>...   remove installed package(s)\n"
           "  vpk list               list installed packages\n"
           "  vpk info    <pkg>      show recipe info\n", VPK_VERSION);
}

int main(int argc, char **argv) {
    if (argc<2){ usage(); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    mkpath(DB_DIR,0755); mkpath(BUILD_DIR,0755);
    const char *cmd=argv[1];
    if      (!strcmp(cmd,"install")&&argc>2) cmd_install(argc-2,argv+2);
    else if (!strcmp(cmd,"remove") &&argc>2) cmd_remove(argc-2,argv+2);
    else if (!strcmp(cmd,"list"))            cmd_list();
    else if (!strcmp(cmd,"info")   &&argc>2) cmd_info(argv[2]);
    else if (!strcmp(cmd,"--version")) printf("vpk %s\n",VPK_VERSION);
    else { usage(); curl_global_cleanup(); return 1; }
    curl_global_cleanup();
    return 0;
}
