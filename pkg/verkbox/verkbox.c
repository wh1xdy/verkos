/* verkbox — VerkOS's own multi-call core utilities.
 *
 * One small, self-contained binary that implements several classic userland
 * tools as "applets" (the BusyBox model): symlink each applet name to this
 * binary and it dispatches on argv[0]. Also callable as `verkbox <applet> ...`.
 *
 * This is the first piece of VerkOS's own userland — the start of replacing the
 * GNU coreutils with tools that are ours, in the same spirit as vpk: plain C,
 * no dependencies, readable end to end. Applets aim to match GNU behaviour for
 * common usage (verified by differential tests against coreutils).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <errno.h>
#include <grp.h>
#include <time.h>
#include <regex.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>

#define VERKBOX_VERSION "0.1"

/* ------------------------------------------------------------- helpers ---- */
/* copy an open stream to stdout; returns 0 on success */
static int slurp(FILE *f) {
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (fwrite(buf, 1, n, stdout) != n) return 1;
    return ferror(f) ? 1 : 0;
}

/* ------------------------------------------------------------- applets ---- */
static int a_true(int c, char **v)  { (void)c; (void)v; return 0; }
static int a_false(int c, char **v) { (void)c; (void)v; return 1; }

static int a_pwd(int c, char **v) {
    (void)c; (void)v;
    char b[4096];
    if (!getcwd(b, sizeof b)) { perror("pwd"); return 1; }
    puts(b);
    return 0;
}

static int a_whoami(int c, char **v) {
    (void)c; (void)v;
    struct passwd *p = getpwuid(geteuid());
    if (!p) { fprintf(stderr, "whoami: cannot find name for uid %u\n", geteuid()); return 1; }
    puts(p->pw_name);
    return 0;
}

static int a_nproc(int c, char **v) {
    (void)c; (void)v;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    printf("%ld\n", n);
    return 0;
}

/* echo [-neE] [args...] */
static int a_echo(int c, char **v) {
    int nflag = 0, eflag = 0, i = 1;
    for (; i < c; i++) {
        if (v[i][0] != '-' || !v[i][1]) break;
        int ok = 1;
        for (char *p = v[i] + 1; *p; p++)
            if (*p != 'n' && *p != 'e' && *p != 'E') { ok = 0; break; }
        if (!ok) break;
        for (char *p = v[i] + 1; *p; p++) {
            if (*p == 'n') nflag = 1;
            else if (*p == 'e') eflag = 1;
            else if (*p == 'E') eflag = 0;
        }
    }
    for (int j = i; j < c; j++) {
        if (j > i) putchar(' ');
        if (!eflag) { fputs(v[j], stdout); continue; }
        for (char *p = v[j]; *p; p++) {
            if (*p != '\\') { putchar(*p); continue; }
            p++;
            switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case 'f': putchar('\f'); break;
                case 'v': putchar('\v'); break;
                case 'e': putchar('\033'); break;
                case 'c': return 0;                 /* stop, no trailing newline */
                case '0': {                         /* \0NNN octal */
                    int val = 0, k = 0; p++;
                    while (k < 3 && *p >= '0' && *p <= '7') { val = val*8 + (*p-'0'); p++; k++; }
                    p--; putchar(val); break;
                }
                case '\0': putchar('\\'); p--; break;
                default: putchar('\\'); putchar(*p); break;
            }
        }
    }
    if (!nflag) putchar('\n');
    return 0;
}

/* cat [files...] ; '-' or no args = stdin */
static int a_cat(int c, char **v) {
    int o_num=0,o_nb=0,o_ends=0,o_tabs=0,o_sq=0,o_np=0, i=1;
    for (; i<c; i++) {
        if (v[i][0]!='-' || !v[i][1]) break;          /* operand, or "-" = stdin */
        if (!strcmp(v[i],"--")) { i++; break; }
        if (v[i][1]=='-') {
            char *o=v[i]+2;
            if(!strcmp(o,"number"))o_num=1; else if(!strcmp(o,"number-nonblank"))o_nb=1;
            else if(!strcmp(o,"show-ends"))o_ends=1; else if(!strcmp(o,"show-tabs"))o_tabs=1;
            else if(!strcmp(o,"squeeze-blank"))o_sq=1; else if(!strcmp(o,"show-nonprinting"))o_np=1;
            else if(!strcmp(o,"show-all")){o_np=o_ends=o_tabs=1;}
            else { fprintf(stderr,"cat: unrecognized option '%s'\n",v[i]); return 1; }
            continue;
        }
        for (char *p=v[i]+1;*p;p++) switch(*p){
            case 'n':o_num=1;break; case 'b':o_nb=1;break; case 'E':o_ends=1;break;
            case 'T':o_tabs=1;break; case 's':o_sq=1;break; case 'v':o_np=1;break;
            case 'A':o_np=o_ends=o_tabs=1;break; case 'e':o_np=o_ends=1;break;
            case 't':o_np=o_tabs=1;break; case 'u':break;
            default: fprintf(stderr,"cat: invalid option -- '%c'\n",*p); return 1;
        }
    }
    if (o_nb) o_num=0;                                 /* -b overrides -n */
    int simple = !(o_num||o_nb||o_ends||o_tabs||o_sq||o_np);
    int nfiles=c-i, rc=0, k=i;
    long lineno=1; int bol=1, blank_run=0;             /* state spans all files (like GNU) */
    do {
        FILE *f; const char *name;
        if (nfiles<=0) { f=stdin; name="-"; }
        else if (!strcmp(v[k],"-")) { f=stdin; name="-"; }
        else { name=v[k]; f=fopen(v[k],"rb"); if(!f){ fprintf(stderr,"cat: %s: %s\n",v[k],strerror(errno)); rc=1; k++; continue; } }
        if (simple) { rc|=slurp(f); if(f!=stdin)fclose(f); k++; continue; }
        int ch;
        while ((ch=getc(f))!=EOF) {
            if (bol) {
                if (o_sq && ch=='\n') { if (++blank_run > 1) continue; }
                else if (ch!='\n') blank_run=0;
                if (o_num || (o_nb && ch!='\n')) printf("%6ld\t", lineno++);
                bol=0;
            }
            if (ch=='\n') { if(o_ends) putchar('$'); putchar('\n'); bol=1; continue; }
            if (ch=='\t') { if(o_tabs){putchar('^');putchar('I');} else putchar('\t'); continue; }
            if (o_np) {
                unsigned uc=(unsigned char)ch;
                if (uc<32) { putchar('^'); putchar(uc+64); }
                else if (uc==127) { putchar('^'); putchar('?'); }
                else if (uc>=128) {
                    putchar('M'); putchar('-'); unsigned d=uc-128;
                    if (d<32){putchar('^');putchar(d+64);} else if(d==127){putchar('^');putchar('?');} else putchar(d);
                } else putchar(ch);
            } else putchar(ch);
        }
        (void)name; if (f!=stdin) fclose(f); k++;
    } while (nfiles>0 && k<c);
    return rc;
}

/* yes [string...] — repeat forever */
static int a_yes(int c, char **v) {
    for (;;) {
        if (c < 2) { if (puts("y") == EOF) return 0; continue; }
        for (int i = 1; i < c; i++) {
            if (fputs(v[i], stdout) == EOF) return 0;
            putchar(i + 1 < c ? ' ' : '\n');
        }
    }
}

/* basename path [suffix] */
static int a_basename(int c, char **v) {
    if (c < 2) { fprintf(stderr, "basename: missing operand\n"); return 1; }
    char *s = v[1]; size_t len = strlen(s);
    while (len > 1 && s[len-1] == '/') s[--len] = 0;      /* strip trailing / */
    char *base = strrchr(s, '/'); base = base ? base + 1 : s;
    if (c > 2) {                                          /* strip suffix */
        size_t bl = strlen(base), sl = strlen(v[2]);
        if (sl && sl < bl && !strcmp(base + bl - sl, v[2])) base[bl - sl] = 0;
    }
    puts(*base ? base : "/");
    return 0;
}

/* dirname path */
static int a_dirname(int c, char **v) {
    if (c < 2) { fprintf(stderr, "dirname: missing operand\n"); return 1; }
    char *s = v[1]; size_t len = strlen(s);
    while (len > 1 && s[len-1] == '/') s[--len] = 0;
    char *slash = strrchr(s, '/');
    if (!slash) { puts("."); return 0; }
    if (slash == s) { puts("/"); return 0; }
    while (slash > s && *(slash-1) == '/') slash--;       /* collapse //// */
    *slash = 0;
    puts(s);
    return 0;
}

/* head [-n N] [files...] — first N lines (default 10) */
static int a_head(int c, char **v) {
    long count = 10; int bytes = 0, quiet = 0, verbose = 0, i = 1, endopts = 0;
    for (; i < c; i++) {
        char *a = v[i];
        if (endopts || a[0] != '-' || !a[1]) break;
        if (!strcmp(a,"--")) { i++; break; }
        else if (!strcmp(a,"-n") && i+1<c) { count=atol(v[++i]); bytes=0; }
        else if (!strcmp(a,"-c") && i+1<c) { count=atol(v[++i]); bytes=1; }
        else if (a[1]=='n' && a[2]) { count=atol(a+2); bytes=0; }
        else if (a[1]=='c' && a[2]) { count=atol(a+2); bytes=1; }
        else if (isdigit((unsigned char)a[1])) { count=atol(a+1); bytes=0; }
        else if (!strcmp(a,"-q")||!strcmp(a,"--quiet")||!strcmp(a,"--silent")) quiet=1;
        else if (!strcmp(a,"-v")||!strcmp(a,"--verbose")) verbose=1;
        else { fprintf(stderr,"head: invalid option -- '%c'\n",a[1]); return 1; }
    }
    int nfiles = c - i, rc = 0, first = 1;
    int headers = verbose || (nfiles > 1 && !quiet);
    if (nfiles <= 0) {
        long k = 0; int ch;
        while (k < count && (ch = getchar()) != EOF) { putchar(ch); k += bytes ? 1 : (ch=='\n'); }
        return 0;
    }
    for (; i < c; i++) {
        int is_stdin = !strcmp(v[i],"-");
        FILE *f = is_stdin ? stdin : fopen(v[i], "rb");
        if (!f) { fprintf(stderr, "head: cannot open '%s' for reading: %s\n", v[i], strerror(errno)); rc = 1; continue; }
        if (headers) { printf("%s==> %s <==\n", first ? "" : "\n", is_stdin?"standard input":v[i]); first = 0; }
        long k = 0; int ch;
        while (k < count && (ch = getc(f)) != EOF) { putchar(ch); k += bytes ? 1 : (ch=='\n'); }
        if (!is_stdin) fclose(f);
    }
    return rc;
}

/* wc [-lwc] [files...] — GNU-compatible column formatting (each count is
 * right-justified to a shared field width = digits of the largest count). */
static int digits(long x) { int d = 1; if (x < 0) x = -x; while (x >= 10) { x /= 10; d++; } return d; }
static int a_wc(int c, char **v) {
    int wl=0, ww=0, wm=0, wc=0, wL=0, i=1;   /* lines words chars bytes max-line */
    for (; i < c; i++) {
        if (v[i][0] != '-' || !v[i][1]) break;
        int ok = 1; for (char *p=v[i]+1;*p;p++) if (*p!='l'&&*p!='w'&&*p!='c'&&*p!='m'&&*p!='L') { ok=0; break; }
        if (!ok) break;
        for (char *p=v[i]+1;*p;p++){ if(*p=='l')wl=1;else if(*p=='w')ww=1;else if(*p=='c')wc=1;else if(*p=='m')wm=1;else if(*p=='L')wL=1; }
    }
    if (!wl&&!ww&&!wc&&!wm&&!wL) { wl=ww=wc=1; }          /* default: lines words bytes */
    int nfiles=c-i, rc=0, n=nfiles>0?nfiles:1;
    long *La=calloc(n,sizeof(long)),*Wa=calloc(n,sizeof(long)),*Ma=calloc(n,sizeof(long)),
         *Ca=calloc(n,sizeof(long)),*Lm=calloc(n,sizeof(long));
    const char **lab=calloc(n,sizeof(char*)); int *okf=calloc(n,sizeof(int));
    if (!La||!Wa||!Ma||!Ca||!Lm||!lab||!okf){ free(La);free(Wa);free(Ma);free(Ca);free(Lm);free(lab);free(okf); return 1; }
    long tl=0,tw=0,tm=0,tc=0,tL=0, maxv=0, maxall=0;
    for (int k=0;k<n;k++) {
        FILE *f=stdin;
        if (nfiles>0){ f=fopen(v[i+k],"rb"); if(!f){fprintf(stderr,"wc: %s: %s\n",v[i+k],strerror(errno));rc=1;continue;} lab[k]=v[i+k]; }
        long L=0,W=0,M=0,C=0,LL=0,col=0; int inword=0,ch;
        while ((ch=getc(f))!=EOF) {
            C++; M++;                                    /* C locale: one char per byte */
            if (ch=='\n'){ L++; if(col>LL)LL=col; col=0; }
            else if (ch=='\t') col=(col/8+1)*8;          /* tab -> next tab stop (GNU -L) */
            else if (isprint((unsigned char)ch)) col++;  /* other control chars: width 0 */
            if (isspace(ch)) inword=0; else if(!inword){ inword=1; W++; }
        }
        if (col>LL) LL=col;                              /* final line w/o newline */
        if (nfiles>0) fclose(f);
        La[k]=L;Wa[k]=W;Ma[k]=M;Ca[k]=C;Lm[k]=LL;okf[k]=1;
        tl+=L;tw+=W;tm+=M;tc+=C; if(LL>tL)tL=LL;
        if(wl&&L>maxv)maxv=L; if(ww&&W>maxv)maxv=W; if(wm&&M>maxv)maxv=M; if(wc&&C>maxv)maxv=C; if(wL&&LL>maxv)maxv=LL;
        if(L>maxall)maxall=L; if(W>maxall)maxall=W; if(M>maxall)maxall=M; if(C>maxall)maxall=C; if(LL>maxall)maxall=LL;
    }
    if (nfiles>1) {
        if(wl&&tl>maxv)maxv=tl; if(ww&&tw>maxv)maxv=tw; if(wm&&tm>maxv)maxv=tm; if(wc&&tc>maxv)maxv=tc; if(wL&&tL>maxv)maxv=tL;
        if(tl>maxall)maxall=tl; if(tw>maxall)maxall=tw; if(tm>maxall)maxall=tm; if(tc>maxall)maxall=tc; if(tL>maxall)maxall=tL;
    }
    int nflags=wl+ww+wm+wc+wL;
    int width=digits(nflags==1?maxv:maxall);
    for (int k=0;k<n;k++) {
        if (nfiles>0 && !okf[k]) continue;
        int first=1;
        if(wl){ if(!first)putchar(' '); printf("%*ld",width,La[k]); first=0; }
        if(ww){ if(!first)putchar(' '); printf("%*ld",width,Wa[k]); first=0; }
        if(wm){ if(!first)putchar(' '); printf("%*ld",width,Ma[k]); first=0; }
        if(wc){ if(!first)putchar(' '); printf("%*ld",width,Ca[k]); first=0; }
        if(wL){ if(!first)putchar(' '); printf("%*ld",width,Lm[k]); first=0; }
        if(lab[k]) printf(" %s",lab[k]);
        putchar('\n');
    }
    if (nfiles>1) {
        int first=1;
        if(wl){ if(!first)putchar(' '); printf("%*ld",width,tl); first=0; }
        if(ww){ if(!first)putchar(' '); printf("%*ld",width,tw); first=0; }
        if(wm){ if(!first)putchar(' '); printf("%*ld",width,tm); first=0; }
        if(wc){ if(!first)putchar(' '); printf("%*ld",width,tc); first=0; }
        if(wL){ if(!first)putchar(' '); printf("%*ld",width,tL); first=0; }
        printf(" total\n");
    }
    free(La);free(Wa);free(Ma);free(Ca);free(Lm);free(lab);free(okf);
    return rc;
}

/* seq [FIRST [STEP]] LAST — integers */
static int a_seq(int c, char **v) {
    long first = 1, step = 1, last;
    if (c == 2) { last = atol(v[1]); }
    else if (c == 3) { first = atol(v[1]); last = atol(v[2]); }
    else if (c == 4) { first = atol(v[1]); step = atol(v[2]); last = atol(v[3]); }
    else { fprintf(stderr, "usage: seq [first [step]] last\n"); return 1; }
    if (step == 0) { fprintf(stderr, "seq: step cannot be 0\n"); return 1; }
    if (step > 0) for (long i = first; i <= last; i += step) printf("%ld\n", i);
    else          for (long i = first; i >= last; i += step) printf("%ld\n", i);
    return 0;
}

/* -------------------------------------------------- extended applets --- */
/* ---- ls ---- */
struct ls_ent { char *name; struct stat st; char *link; int skip; };
static int ls_all, ls_almost, ls_lng, ls_recurse, ls_rev, ls_bytime, ls_bysize, ls_dirself, ls_human, ls_class, ls_inode;

static int ls_namecmp(const void *a, const void *b) {
    const struct ls_ent *x = a, *y = b; int r = strcmp(x->name, y->name);
    return ls_rev ? -r : r;
}
static int ls_pathcmp(const void *a, const void *b) {   /* sort directory operands */
    int r = strcmp(*(char *const *)a, *(char *const *)b);
    return ls_rev ? -r : r;
}
static int ls_timecmp(const void *a, const void *b) {
    const struct ls_ent *x = a, *y = b;
    /* newest first; compare at nanosecond resolution like GNU (else same-second
     * entries tie-break differently). */
    long xs=x->st.st_mtim.tv_sec, ys=y->st.st_mtim.tv_sec;
    int r = (xs < ys) - (xs > ys);
    if (!r) { long xn=x->st.st_mtim.tv_nsec, yn=y->st.st_mtim.tv_nsec; r = (xn < yn) - (xn > yn); }
    if (!r) r = strcmp(x->name, y->name);
    return ls_rev ? -r : r;
}
static int ls_sizecmp(const void *a, const void *b) {
    const struct ls_ent *x = a, *y = b;
    int r = (x->st.st_size < y->st.st_size) - (x->st.st_size > y->st.st_size); /* largest first */
    if (!r) r = strcmp(x->name, y->name);
    return ls_rev ? -r : r;
}
static void ls_sort(struct ls_ent *e, int n) {
    qsort(e, n, sizeof *e, ls_bytime ? ls_timecmp : ls_bysize ? ls_sizecmp : ls_namecmp);
}
static void ls_modestr(mode_t m, char *s) {
    s[0] = S_ISDIR(m)?'d':S_ISLNK(m)?'l':S_ISCHR(m)?'c':S_ISBLK(m)?'b':S_ISFIFO(m)?'p':S_ISSOCK(m)?'s':'-';
    s[1]=(m&S_IRUSR)?'r':'-'; s[2]=(m&S_IWUSR)?'w':'-';
    s[3]=(m&S_ISUID)?((m&S_IXUSR)?'s':'S'):((m&S_IXUSR)?'x':'-');
    s[4]=(m&S_IRGRP)?'r':'-'; s[5]=(m&S_IWGRP)?'w':'-';
    s[6]=(m&S_ISGID)?((m&S_IXGRP)?'s':'S'):((m&S_IXGRP)?'x':'-');
    s[7]=(m&S_IROTH)?'r':'-'; s[8]=(m&S_IWOTH)?'w':'-';
    s[9]=(m&S_ISVTX)?((m&S_IXOTH)?'t':'T'):((m&S_IXOTH)?'x':'-');
    s[10]=0;
}
static void ls_humansize(long long v, char *out) {
    static const char u[] = "\0KMGTPE"; double d = v; int i = 0;
    if (v < 1024) { sprintf(out, "%lld", v); return; }
    while (d >= 1024 && u[i+1]) { d /= 1024; i++; }
    if (d < 10) { double r = (double)((long long)(d*10 + 0.99999))/10.0; sprintf(out, "%.1f%c", r, u[i]); }
    else        { long long r = (long long)(d + 0.99999); sprintf(out, "%lld%c", r, u[i]); }
}
static const char *ls_uname(uid_t u){ struct passwd *p=getpwuid(u); static char b[32]; if(p) return p->pw_name; sprintf(b,"%u",(unsigned)u); return b; }
static const char *ls_gname(gid_t g){ struct group  *p=getgrgid(g); static char b[32]; if(p) return p->gr_name; sprintf(b,"%u",(unsigned)g); return b; }
static void ls_datestr(time_t t, char *out) {
    time_t now = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char mon[8]; strftime(mon, sizeof mon, "%b", &tm);
    int recent = (now - t < 15778476) && (t - now < 3600);
    if (recent) sprintf(out, "%s %2d %02d:%02d", mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
    else        sprintf(out, "%s %2d  %d", mon, tm.tm_mday, tm.tm_year + 1900);
}
static char ls_typechar(const struct stat *st) {
    mode_t m = st->st_mode;
    if (S_ISDIR(m)) return '/';
    if (S_ISLNK(m)) return '@';
    if (S_ISFIFO(m)) return '|';
    if (S_ISSOCK(m)) return '=';
    if (m & (S_IXUSR|S_IXGRP|S_IXOTH)) return '*';
    return 0;
}
/* print an already-sorted set of entries. show_total: emit the "total N" line
 * (directory listings do; a group of file operands does not). */
static void ls_print(struct ls_ent *e, int n, int show_total) {
    /* Two passes with O(1) stack (no per-entry VLAs — those overflow the stack on
     * a directory with enough entries, and ls is attacker-facing since anyone can
     * create files). Pass 1: column widths + block total. Pass 2: format each row
     * into small fixed buffers, resolving owner/group on the fly. */
    int wi=1, wn=1, wo=1, wg=1, ws=1; long long total=0;
    char b[64];
    for (int i=0;i<n;i++) {
        if (ls_inode) { int l=snprintf(b,sizeof b,"%lu",(unsigned long)e[i].st.st_ino); if(l>wi)wi=l; }
        if (ls_lng) {
            int l;
            l=snprintf(b,sizeof b,"%lu",(unsigned long)e[i].st.st_nlink); if(l>wn)wn=l;
            l=(int)strlen(ls_uname(e[i].st.st_uid)); if(l>wo)wo=l;
            l=(int)strlen(ls_gname(e[i].st.st_gid)); if(l>wg)wg=l;
            if (ls_human) ls_humansize((long long)e[i].st.st_size, b);
            else snprintf(b,sizeof b,"%lld",(long long)e[i].st.st_size);
            l=(int)strlen(b); if(l>ws)ws=l;
            total += (long long)e[i].st.st_blocks;
        }
    }
    if (ls_lng && show_total) {
        if (ls_human) { char tb[32]; ls_humansize((total/2)*1024, tb); printf("total %s\n", tb); }
        else printf("total %lld\n", total/2);
    }
    for (int i=0;i<n;i++) {
        if (e[i].skip) continue;   /* dir operand: counted for width, not printed */
        if (ls_inode) printf("%*lu ", wi, (unsigned long)e[i].st.st_ino);
        if (ls_lng) {
            char modes[11], dates[40], sizes[32];
            ls_modestr(e[i].st.st_mode, modes);
            if (ls_human) ls_humansize((long long)e[i].st.st_size, sizes);
            else snprintf(sizes,sizeof sizes,"%lld",(long long)e[i].st.st_size);
            ls_datestr(e[i].st.st_mtime, dates);
            /* owner then group: ls_uname/ls_gname use distinct static buffers, so
             * both stay valid across one printf. */
            printf("%s %*lu %-*s ", modes, wn,(unsigned long)e[i].st.st_nlink, wo, ls_uname(e[i].st.st_uid));
            printf("%-*s %*s %s %s", wg, ls_gname(e[i].st.st_gid), ws, sizes, dates, e[i].name);
            if (e[i].link) printf(" -> %s", e[i].link);
            else if (ls_class) { char c=ls_typechar(&e[i].st); if(c&&c!='@') putchar(c); }
            putchar('\n');
        } else {
            fputs(e[i].name, stdout);
            if (ls_class) { char c=ls_typechar(&e[i].st); if(c) putchar(c); }
            putchar('\n');
        }
    }
}
/* list a directory. print_header: emit "path:" header. first: is this the first
 * output block (suppresses the leading blank line before the header). */
static int ls_dir(const char *path, int print_header, int first) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "ls: cannot open directory '%s': %s\n", path, strerror(errno)); return 2; }
    struct ls_ent *e = NULL; int n = 0, cap = 0, rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0]=='.') {
            if (ls_all) { }
            else if (ls_almost) { if (!strcmp(nm,".")||!strcmp(nm,"..")) continue; }
            else continue;
        }
        if (n==cap) {
            int ncap = cap ? cap*2 : 32;
            struct ls_ent *ne = realloc(e, (size_t)ncap*sizeof *e);
            if (!ne) { for(int j=0;j<n;j++){free(e[j].name);free(e[j].link);} free(e); closedir(d);
                       fprintf(stderr,"ls: out of memory\n"); return 2; }
            e = ne; cap = ncap;
        }
        char full[8192]; snprintf(full,sizeof full,"%s/%s",path,nm);
        e[n].name = strdup(nm); e[n].link = NULL; e[n].skip = 0;
        if (lstat(full,&e[n].st)!=0) memset(&e[n].st,0,sizeof e[n].st);
        if (S_ISLNK(e[n].st.st_mode) && ls_lng) {
            char t[4096]; ssize_t k=readlink(full,t,sizeof t-1); if(k>=0){ t[k]=0; e[n].link=strdup(t); }
        }
        n++;
    }
    closedir(d);
    ls_sort(e,n);
    if (print_header) { if(!first) putchar('\n'); printf("%s:\n", path); }
    ls_print(e,n,1);
    if (ls_recurse) {
        for (int i=0;i<n;i++) {
            if (!strcmp(e[i].name,".")||!strcmp(e[i].name,"..")) continue;
            if (S_ISDIR(e[i].st.st_mode)) {
                char sub[8192]; snprintf(sub,sizeof sub,"%s/%s",path,e[i].name);
                rc |= ls_dir(sub,1,0) ? 2 : 0;    /* leading blank + header */
            }
        }
    }
    for (int i=0;i<n;i++){ free(e[i].name); free(e[i].link); }
    free(e);
    return rc;
}
static int a_ls(int argc, char **argv) {
    ls_all=ls_almost=ls_lng=ls_recurse=ls_rev=ls_bytime=ls_bysize=ls_dirself=ls_human=ls_class=ls_inode=0;
    int i, endopts=0, status=0;
    char **ops = malloc(sizeof(char*)*(argc>0?argc:1)); int nop=0;
    for (i=1;i<argc;i++) {
        char *a=argv[i];
        if (!endopts && a[0]=='-' && a[1]) {
            if (a[1]=='-'&&!a[2]) { endopts=1; continue; }
            if (a[1]=='-') {
                if(!strcmp(a,"--all"))ls_all=1; else if(!strcmp(a,"--almost-all"))ls_almost=1;
                else if(!strcmp(a,"--recursive"))ls_recurse=1; else if(!strcmp(a,"--reverse"))ls_rev=1;
                else if(!strcmp(a,"--human-readable"))ls_human=1; else if(!strcmp(a,"--classify"))ls_class=1;
                else if(!strcmp(a,"--inode"))ls_inode=1;
                else if(!strncmp(a,"--color",7)) { /* we don't colorize: accept & ignore */ }
                else { fprintf(stderr,"ls: unrecognized option '%s'\n",a); free(ops); return 2; }
                continue;
            }
            for (char *p=a+1;*p;p++) switch(*p){
                case 'a':ls_all=1;break; case 'A':ls_almost=1;break; case 'l':ls_lng=1;break;
                case 'R':ls_recurse=1;break; case 'r':ls_rev=1;break; case 't':ls_bytime=1;break;
                case 'S':ls_bysize=1;break; case 'd':ls_dirself=1;break; case 'h':ls_human=1;break;
                case 'F':ls_class=1;break; case 'i':ls_inode=1;break; case '1':break;
                default: fprintf(stderr,"ls: invalid option -- '%c'\n",*p); free(ops); return 2;
            }
            continue;
        }
        ops[nop++]=a;
    }
    struct ls_ent *files = malloc(sizeof(struct ls_ent)*(nop?nop:1)); int nf=0;
    char **dirs = malloc(sizeof(char*)*(nop?nop:1)); int nd=0;
    if (nop==0) {
        if (ls_dirself){ files[nf].name=strdup("."); files[nf].link=NULL; files[nf].skip=0; lstat(".",&files[nf].st); nf++; }
        else dirs[nd++]=".";
    }
    for (i=0;i<nop;i++) {
        struct stat st;
        if (lstat(ops[i],&st)!=0){ fprintf(stderr,"ls: cannot access '%s': %s\n",ops[i],strerror(errno)); status=2; continue; }
        int isdir = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode)) { struct stat s2; if(stat(ops[i],&s2)==0) isdir=S_ISDIR(s2.st_mode); }
        files[nf].name=strdup(ops[i]); files[nf].link=NULL; files[nf].st=st;
        if (isdir && !ls_dirself) {
            dirs[nd++]=ops[i];        /* listed separately; kept in files[] (skip) so its
                                         size/nlink still size the file-block columns like GNU */
            files[nf].skip=1;
        } else {
            files[nf].skip=0;
            if(S_ISLNK(st.st_mode)&&ls_lng){char t[4096];ssize_t k=readlink(ops[i],t,sizeof t-1);if(k>=0){t[k]=0;files[nf].link=strdup(t);}}
        }
        nf++;
    }
    ls_sort(files,nf);
    if (nd > 1) qsort(dirs, nd, sizeof(char*), ls_pathcmp);   /* GNU sorts operands */
    int nf_real=0; for(i=0;i<nf;i++) if(!files[i].skip) nf_real++;
    int hdr = (((nf_real?1:0)+nd) > 1) || ls_recurse;
    if (nf) ls_print(files,nf,0);
    int first = (nf_real==0);
    for (i=0;i<nd;i++) { status |= ls_dir(dirs[i], hdr, first) ? 2 : 0; first=0; }
    for (i=0;i<nf;i++){ free(files[i].name); free(files[i].link); }
    free(files); free(dirs); free(ops);
    return status;
}

/* ---- tail ---- */
/* tail [-n N|-N|-n +K] [file...] — print the last N lines (default 10).
 * -n +K prints from line K to end. With >1 file, GNU-style "==> name <=="
 * headers separated by a blank line. Last-N mode keeps only min(N,#lines)
 * lines in memory via a growing ring buffer. */
static char *tail_readline(FILE *f, size_t *outlen) {
    size_t cap = 128, len = 0;
    char *buf = malloc(cap);
    if (!buf) { *outlen = 0; return NULL; }
    int c;
    while ((c = getc(f)) != EOF) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); *outlen = 0; return NULL; }
            buf = nb; cap *= 2;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) { free(buf); *outlen = 0; return NULL; }
    *outlen = len;
    return buf;
}

static int tail_parsecount(const char *s, long *count, int *from_start) {
    const char *p = s;
    if (*p == '+') { *from_start = 1; p++; }
    else { *from_start = 0; if (*p == '-') p++; }
    if (*p == '\0') return -1;
    long v = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        if (v < 100000000000000000L)
            v = v * 10 + (*p - '0');
    }
    *count = v;
    return 0;
}

static int tail_stream(FILE *f, long n, int from_start) {
    size_t len;
    char *line;
    if (from_start) {                       /* -n +K: print from line K */
        long ln = 0;
        while ((line = tail_readline(f, &len)) != NULL) {
            ln++;
            if (ln >= n) fwrite(line, 1, len, stdout);
            free(line);
        }
        return 0;
    }
    if (n <= 0) return 0;                    /* -n 0: nothing */
    char **lines = NULL;
    size_t *lens = NULL;
    long cap = 0, fill = 0, start = 0;
    while ((line = tail_readline(f, &len)) != NULL) {
        if (fill < n) {                     /* filling phase: array grows up to n */
            if (fill == cap) {
                long ncap = cap ? cap * 2 : 16;
                if (ncap > n) ncap = n;
                char **nl = realloc(lines, (size_t)ncap * sizeof(char *));
                if (!nl) {
                    for (long k = 0; k < fill; k++) free(lines[k]);
                    free(lines); free(lens); free(line); return 1;
                }
                lines = nl;
                size_t *nn = realloc(lens, (size_t)ncap * sizeof(size_t));
                if (!nn) {
                    for (long k = 0; k < fill; k++) free(lines[k]);
                    free(lines); free(lens); free(line); return 1;
                }
                lens = nn;
                cap = ncap;
            }
            lines[fill] = line;
            lens[fill] = len;
            fill++;
        } else {                            /* full: overwrite oldest (ring) */
            free(lines[start]);
            lines[start] = line;
            lens[start] = len;
            start = (start + 1) % n;
        }
    }
    for (long k = 0; k < fill; k++) {
        long idx = (fill == n) ? (start + k) % n : k;
        fwrite(lines[idx], 1, lens[idx], stdout);
    }
    for (long k = 0; k < fill; k++) free(lines[k]);
    free(lines);
    free(lens);
    return 0;
}

/* emit the last n bytes, or (from_start) from byte n, of stream f */
static int tail_bytes(FILE *f, long n, int from_start) {
    int ch;
    if (from_start) { long pos=0; while((ch=getc(f))!=EOF){ if(++pos>=n) putchar(ch); } return 0; }
    if (n <= 0) return 0;
    char *buf = malloc((size_t)n); if(!buf) return 1;
    long fill=0, start=0;
    while ((ch=getc(f))!=EOF) { if(fill<n) buf[fill++]=(char)ch; else { buf[start]=(char)ch; start=(start+1)%n; } }
    for (long k=0;k<fill;k++) putchar(buf[(start+k)%fill]);
    free(buf); return 0;
}
static int a_tail(int argc, char **argv) {
    long count = 10;
    int from_start = 0, bytes = 0, quiet = 0, verbose = 0;
    int end_opts = 0;
    char **files = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(char *));
    if (!files) { fprintf(stderr, "tail: memory exhausted\n"); return 1; }
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!end_opts && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "--") == 0) { end_opts = 1; continue; }
            if (arg[1] == 'n' || arg[1] == 'c') {
                int b = (arg[1] == 'c');
                const char *spec;
                if (arg[2] != '\0') spec = arg + 2;         /* -nN / -cN / -n+K */
                else {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "tail: option requires an argument -- '%c'\n", arg[1]);
                        free(files); return 1;
                    }
                    spec = argv[++i];                       /* -n N / -c N */
                }
                if (tail_parsecount(spec, &count, &from_start) != 0) {
                    fprintf(stderr, "tail: invalid number: '%s'\n", spec);
                    free(files); return 1;
                }
                bytes = b;
            } else if (!strcmp(arg,"-q")||!strcmp(arg,"--quiet")||!strcmp(arg,"--silent")) { quiet=1;
            } else if (!strcmp(arg,"-v")||!strcmp(arg,"--verbose")) { verbose=1;
            } else if (isdigit((unsigned char)arg[1])) {
                if (tail_parsecount(arg + 1, &count, &from_start) != 0) {  /* -N */
                    fprintf(stderr, "tail: invalid number of lines: '%s'\n", arg + 1);
                    free(files); return 1;
                }
            } else {
                fprintf(stderr, "tail: invalid option -- '%c'\n", arg[1]);
                free(files); return 1;
            }
        } else {
            files[nfiles++] = arg;
        }
    }

    int ret = 0;
    int headers = verbose || (nfiles > 1 && !quiet);
    if (nfiles == 0) {
        if (headers) printf("==> standard input <==\n");
        ret = bytes ? tail_bytes(stdin, count, from_start) : tail_stream(stdin, count, from_start);
    } else {
        int printed_header = 0;
        for (int fi = 0; fi < nfiles; fi++) {
            const char *fn = files[fi];
            FILE *f;
            int is_stdin = 0;
            if (strcmp(fn, "-") == 0) { f = stdin; is_stdin = 1; }
            else {
                f = fopen(fn, "r");
                if (!f) {
                    fprintf(stderr, "tail: cannot open '%s' for reading: %s\n",
                            fn, strerror(errno));
                    ret = 1;
                    continue;
                }
            }
            if (headers) {
                printf("%s==> %s <==\n", printed_header ? "\n" : "",
                       is_stdin ? "standard input" : fn);
                printed_header = 1;
            }
            if ((bytes ? tail_bytes(f, count, from_start) : tail_stream(f, count, from_start)) != 0) ret = 1;
            if (!is_stdin) fclose(f);
        }
    }
    free(files);
    return ret;
}

/* ---- cut ---- */
static int cut_selected(int pos, const int *los, const int *his, int nr)
{
    int k;
    for (k = 0; k < nr; k++)
        if (pos >= los[k] && pos <= his[k])
            return 1;
    return 0;
}

static int cut_optis(const char *opt, size_t olen, const char *name)
{
    return olen == strlen(name) && strncmp(opt, name, olen) == 0;
}

static int cut_parse_list(const char *name, const char *spec,
                          int **plos, int **phis)
{
    const char *p;
    int cap = 1, cnt = 0;
    int *los, *his;

    for (p = spec; *p; p++)
        if (*p == ',')
            cap++;
    los = malloc(sizeof(int) * cap);
    his = malloc(sizeof(int) * cap);
    if (!los || !his) {
        free(los);
        free(his);
        fprintf(stderr, "%s: memory exhausted\n", name);
        return -1;
    }

    p = spec;
    if (*p == '\0') {
        fprintf(stderr, "%s: invalid byte, character or field list\n", name);
        free(los);
        free(his);
        return -1;
    }

    while (*p) {
        long lo = 0, hi = 0, L, H;
        int haslo = 0, hashi = 0, dash = 0;

        while (isdigit((unsigned char)*p)) {
            lo = lo * 10 + (*p - '0');
            if (lo > INT_MAX)
                lo = INT_MAX;
            haslo = 1;
            p++;
        }
        if (*p == '-') {
            dash = 1;
            p++;
            while (isdigit((unsigned char)*p)) {
                hi = hi * 10 + (*p - '0');
                if (hi > INT_MAX)
                    hi = INT_MAX;
                hashi = 1;
                p++;
            }
        }

        if (dash) {
            if (!haslo && !hashi) {
                fprintf(stderr, "%s: invalid range with no endpoint: -\n", name);
                free(los);
                free(his);
                return -1;
            }
            L = haslo ? lo : 1;
            H = hashi ? hi : INT_MAX;
        } else {
            if (!haslo) {
                fprintf(stderr, "%s: invalid byte, character or field list\n", name);
                free(los);
                free(his);
                return -1;
            }
            L = lo;
            H = lo;
        }

        if (L == 0 || H == 0) {
            fprintf(stderr, "%s: fields and positions are numbered from 1\n", name);
            free(los);
            free(his);
            return -1;
        }
        if (L > H) {
            fprintf(stderr, "%s: invalid decreasing range\n", name);
            free(los);
            free(his);
            return -1;
        }

        los[cnt] = (int)L;
        his[cnt] = (int)H;
        cnt++;

        if (*p == ',') {
            p++;
            continue;
        } else if (*p == '\0') {
            break;
        } else {
            fprintf(stderr, "%s: invalid byte, character or field list\n", name);
            free(los);
            free(his);
            return -1;
        }
    }

    *plos = los;
    *phis = his;
    return cnt;
}

static void cut_stream(FILE *f, int mode, char delim, int only_delim,
                       const int *los, const int *his, int nr)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, f)) != -1) {
        size_t len = (size_t)n;
        if (len > 0 && line[len - 1] == '\n')
            len--;

        if (mode == 'f') {
            size_t j;
            int has = 0;
            for (j = 0; j < len; j++)
                if (line[j] == delim) {
                    has = 1;
                    break;
                }
            if (!has) {
                if (!only_delim) {
                    fwrite(line, 1, len, stdout);
                    putchar('\n');
                }
            } else {
                int fieldnum = 0, printed = 0;
                size_t start = 0;
                for (j = 0; j <= len; j++) {
                    if (j == len || line[j] == delim) {
                        fieldnum++;
                        if (cut_selected(fieldnum, los, his, nr)) {
                            if (printed)
                                putchar(delim);
                            fwrite(line + start, 1, j - start, stdout);
                            printed = 1;
                        }
                        start = j + 1;
                    }
                }
                putchar('\n');
            }
        } else {
            size_t j;
            for (j = 0; j < len; j++)
                if (cut_selected((int)(j + 1), los, his, nr))
                    putchar(line[j]);
            putchar('\n');
        }
    }
    free(line);
}

static int a_cut(int argc, char **argv)
{
    int mode = 0;
    char delim = '\t';
    int delim_set = 0, only_delim = 0;
    char *list_str = NULL;
    int i, rc = 0, nfiles = 0, end_opts = 0, nr;
    int *los = NULL, *his = NULL;
    char **files;

    files = malloc(sizeof(char *) * (argc + 1));
    if (!files) {
        fprintf(stderr, "%s: memory exhausted\n", argv[0]);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!end_opts && arg[0] == '-' && arg[1] != '\0') {
            if (arg[1] == '-') {
                char *opt = arg + 2, *val = NULL, *eq;
                size_t olen;
                if (*opt == '\0') {
                    end_opts = 1;
                    continue;
                }
                eq = strchr(opt, '=');
                if (eq) {
                    olen = (size_t)(eq - opt);
                    val = eq + 1;
                } else {
                    olen = strlen(opt);
                }

                if (cut_optis(opt, olen, "fields") ||
                    cut_optis(opt, olen, "characters") ||
                    cut_optis(opt, olen, "bytes") ||
                    cut_optis(opt, olen, "delimiter")) {
                    char *v;
                    if (val) {
                        v = val;
                    } else {
                        i++;
                        if (i >= argc) {
                            fprintf(stderr,
                                    "%s: option '--%.*s' requires an argument\n",
                                    argv[0], (int)olen, opt);
                            free(files);
                            return 1;
                        }
                        v = argv[i];
                    }
                    if (cut_optis(opt, olen, "delimiter")) {
                        if (strlen(v) != 1) {
                            fprintf(stderr,
                                    "%s: the delimiter must be a single character\n",
                                    argv[0]);
                            free(files);
                            return 1;
                        }
                        delim = v[0];
                        delim_set = 1;
                    } else {
                        int m = cut_optis(opt, olen, "fields") ? 'f' : 'c';
                        if (mode && mode != m) {
                            fprintf(stderr,
                                    "%s: only one type of list may be specified\n",
                                    argv[0]);
                            free(files);
                            return 1;
                        }
                        mode = m;
                        list_str = v;
                    }
                } else if (cut_optis(opt, olen, "only-delimited")) {
                    only_delim = 1;
                } else {
                    fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                            argv[0], (int)olen, opt);
                    free(files);
                    return 1;
                }
                continue;
            } else {
                char *p = arg + 1;
                while (*p) {
                    char c = *p;
                    if (c == 's') {
                        only_delim = 1;
                        p++;
                        continue;
                    }
                    if (c == 'n') {
                        p++;
                        continue;
                    }
                    if (c == 'c' || c == 'f' || c == 'b' || c == 'd') {
                        char *val;
                        if (*(p + 1)) {
                            val = p + 1;
                        } else {
                            i++;
                            if (i >= argc) {
                                fprintf(stderr,
                                        "%s: option requires an argument -- '%c'\n",
                                        argv[0], c);
                                free(files);
                                return 1;
                            }
                            val = argv[i];
                        }
                        if (c == 'd') {
                            if (strlen(val) != 1) {
                                fprintf(stderr,
                                        "%s: the delimiter must be a single character\n",
                                        argv[0]);
                                free(files);
                                return 1;
                            }
                            delim = val[0];
                            delim_set = 1;
                        } else {
                            int m = (c == 'f') ? 'f' : 'c';
                            if (mode && mode != m) {
                                fprintf(stderr,
                                        "%s: only one type of list may be specified\n",
                                        argv[0]);
                                free(files);
                                return 1;
                            }
                            mode = m;
                            list_str = val;
                        }
                        break;
                    }
                    fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
                    free(files);
                    return 1;
                }
            }
        } else {
            files[nfiles++] = arg;
        }
    }

    if (mode == 0) {
        fprintf(stderr,
                "%s: you must specify a list of bytes, characters, or fields\n",
                argv[0]);
        free(files);
        return 1;
    }
    if (delim_set && mode != 'f') {
        fprintf(stderr,
                "%s: an input delimiter may be specified only when operating on fields\n",
                argv[0]);
        free(files);
        return 1;
    }
    if (only_delim && mode != 'f') {
        fprintf(stderr,
                "%s: suppressing non-delimited lines makes sense\n"
                "\tonly when operating on fields\n",
                argv[0]);
        free(files);
        return 1;
    }

    nr = cut_parse_list(argv[0], list_str, &los, &his);
    if (nr < 0) {
        free(files);
        return 1;
    }

    if (nfiles == 0) {
        cut_stream(stdin, mode, delim, only_delim, los, his, nr);
    } else {
        int k;
        for (k = 0; k < nfiles; k++) {
            FILE *f;
            if (strcmp(files[k], "-") == 0) {
                f = stdin;
            } else {
                f = fopen(files[k], "r");
                if (!f) {
                    fprintf(stderr, "%s: %s: %s\n", argv[0], files[k],
                            strerror(errno));
                    rc = 1;
                    continue;
                }
            }
            cut_stream(f, mode, delim, only_delim, los, his, nr);
            if (f != stdin)
                fclose(f);
        }
    }

    free(los);
    free(his);
    free(files);
    return rc;
}

/* ---- rev ---- */
static int rev_stream(const char *name, FILE *f)
{
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int c, rc = 0;

    for (;;) {
        c = getc(f);
        if (c == '\n' || c == EOF) {
            /* reverse buf[0..len) in place, byte-wise */
            if (len) {
                size_t i = 0, j = len - 1;
                while (i < j) {
                    char t = buf[i];
                    buf[i] = buf[j];
                    buf[j] = t;
                    i++;
                    j--;
                }
                fwrite(buf, 1, len, stdout);
            }
            if (c == '\n')
                putchar('\n');
            len = 0;
            if (c == EOF)
                break;
        } else {
            if (len + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 128;
                char *nb = realloc(buf, ncap);
                if (!nb) {
                    free(buf);
                    fprintf(stderr, "rev: memory exhausted\n");
                    return 1;
                }
                buf = nb;
                cap = ncap;
            }
            buf[len++] = (char)c;
        }
    }

    free(buf);
    if (ferror(f)) {
        fprintf(stderr, "rev: %s: %s\n", name, strerror(errno));
        rc = 1;
    }
    return rc;
}

static int a_rev(int argc, char **argv)
{
    int rc = 0, i;

    if (argc < 2)
        return rev_stream("stdin", stdin);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (rev_stream("stdin", stdin))
                rc = 1;
            continue;
        }
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "rev: cannot open %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (rev_stream(argv[i], f))
            rc = 1;
        fclose(f);
    }
    return rc;
}

/* ---- uniq ---- */
static void uniq_write(FILE *out, int cflag, int print_uniq, int print_dup,
                       const char *line, size_t len, unsigned long gcount)
{
    if (gcount == 1UL ? print_uniq : print_dup) {
        if (cflag)
            fprintf(out, "%7lu ", gcount);
        fwrite(line, 1, len, out);
    }
}

static int a_uniq(int argc, char **argv)
{
    int cflag = 0, dflag = 0, uflag = 0;
    int i = 1;

    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0')
            break;                     /* non-option or "-" (stdin) */
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[1] == '-') {             /* long option */
            if (strcmp(a, "--count") == 0) cflag = 1;
            else if (strcmp(a, "--repeated") == 0) dflag = 1;
            else if (strcmp(a, "--unique") == 0) uflag = 1;
            else {
                fprintf(stderr, "uniq: unrecognized option '%s'\n", a);
                return 1;
            }
            continue;
        }
        for (char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'c': cflag = 1; break;
            case 'd': dflag = 1; break;
            case 'u': uflag = 1; break;
            default:
                fprintf(stderr, "uniq: invalid option -- '%c'\n", *p);
                return 1;
            }
        }
    }

    const char *infile = NULL, *outfile = NULL;
    if (i < argc) infile = argv[i++];
    if (i < argc) outfile = argv[i++];
    if (i < argc) {
        fprintf(stderr, "uniq: extra operand '%s'\n", argv[i]);
        return 1;
    }

    FILE *in = stdin, *out = stdout;
    if (infile && strcmp(infile, "-") != 0) {
        in = fopen(infile, "r");
        if (!in) {
            fprintf(stderr, "uniq: %s: %s\n", infile, strerror(errno));
            return 1;
        }
    }
    if (outfile && strcmp(outfile, "-") != 0) {
        out = fopen(outfile, "w");
        if (!out) {
            fprintf(stderr, "uniq: %s: %s\n", outfile, strerror(errno));
            if (in != stdin) fclose(in);
            return 1;
        }
    }

    int print_uniq = !dflag;   /* print groups that occur exactly once */
    int print_dup  = !uflag;   /* print groups that occur more than once */

    char *prev = NULL, *cur = NULL;
    size_t prevcap = 0, curcap = 0;
    ssize_t prevlen, curlen;
    unsigned long count = 0;   /* additional matches after the first in a group */
    int rc = 0;

    prevlen = getline(&prev, &prevcap, in);
    if (prevlen >= 0) {
        while ((curlen = getline(&cur, &curcap, in)) >= 0) {
            if (curlen == prevlen && memcmp(cur, prev, (size_t)curlen) == 0) {
                count++;
            } else {
                uniq_write(out, cflag, print_uniq, print_dup,
                           prev, (size_t)prevlen, count + 1);
                char *tb = prev; prev = cur; cur = tb;
                size_t tc = prevcap; prevcap = curcap; curcap = tc;
                prevlen = curlen;
                count = 0;
            }
        }
        uniq_write(out, cflag, print_uniq, print_dup,
                   prev, (size_t)prevlen, count + 1);
    }

    free(prev);
    free(cur);

    if (ferror(in)) {
        fprintf(stderr, "uniq: %s: %s\n", infile ? infile : "-", strerror(errno));
        rc = 1;
    }
    if (fflush(out) != 0 || ferror(out)) {
        fprintf(stderr, "uniq: write error: %s\n", strerror(errno));
        rc = 1;
    }

    if (in != stdin) fclose(in);
    if (out != stdout) fclose(out);
    return rc;
}

/* ---- mkdir ---- */
static int mkdir_one(char *buf, mode_t mode, int parents)
{
    if (parents) {
        char *p = buf;
        if (*p == '/') p++;
        for (; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                    fprintf(stderr, "mkdir: cannot create directory '%s': %s\n",
                            buf, strerror(errno));
                    return 1;
                }
                *p = '/';
                while (p[1] == '/') p++;
            }
        }
    }
    if (mkdir(buf, mode) != 0) {
        if (parents && errno == EEXIST) {
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
                return 0;
        }
        fprintf(stderr, "mkdir: cannot create directory '%s': %s\n",
                buf, strerror(errno));
        return 1;
    }
    return 0;
}

static int a_mkdir(int argc, char **argv)
{
    int parents = 0;
    int i = 1;

    for (; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0')
            break;
        if (strcmp(arg, "--") == 0) { i++; break; }
        if (strcmp(arg, "--parents") == 0) { parents = 1; continue; }
        for (char *c = arg + 1; *c; c++) {
            if (*c == 'p') {
                parents = 1;
            } else {
                fprintf(stderr, "mkdir: invalid option -- '%c'\n", *c);
                fprintf(stderr, "Try 'mkdir --help' for more information.\n");
                return 1;
            }
        }
    }

    if (i >= argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 1;
    }

    int rc = 0;
    for (; i < argc; i++) {
        if (mkdir_one(argv[i], 0777, parents) != 0)
            rc = 1;
    }
    return rc;
}

/* ---- rmdir ---- */
static void rmdir_strip(char *p) {
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') { p[len - 1] = '\0'; len--; }
}

static int rmdir_parents_fn(char *dir, int verbose, int ignore_ne) {
    char *slash;
    rmdir_strip(dir);
    while ((slash = strrchr(dir, '/')) != NULL) {
        while (slash > dir && *slash == '/') --slash;
        slash[1] = '\0';
        if (verbose)
            printf("rmdir: removing directory, '%s'\n", dir);
        if (rmdir(dir) != 0) {
            if (ignore_ne && (errno == ENOTEMPTY || errno == EEXIST))
                return 1;
            fprintf(stderr, "rmdir: failed to remove '%s': %s\n", dir, strerror(errno));
            return 0;
        }
    }
    return 1;
}

static int a_rmdir(int argc, char **argv) {
    int parents = 0, verbose = 0, ignore_ne = 0;
    int i, ok = 1, endopts = 0, nops = 0;
    char **ops = malloc(sizeof(char *) * (argc > 0 ? (size_t)argc : 1));

    if (!ops) {
        fprintf(stderr, "rmdir: memory exhausted\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopts && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-') {
                char *o = a + 2;
                if (*o == '\0') { endopts = 1; continue; }
                if (!strcmp(o, "parents")) parents = 1;
                else if (!strcmp(o, "verbose")) verbose = 1;
                else if (!strcmp(o, "ignore-fail-on-non-empty")) ignore_ne = 1;
                else {
                    fprintf(stderr, "rmdir: unrecognized option '%s'\n", a);
                    fprintf(stderr, "Try 'rmdir --help' for more information.\n");
                    free(ops);
                    return 1;
                }
            } else {
                char *c;
                for (c = a + 1; *c; c++) {
                    switch (*c) {
                        case 'p': parents = 1; break;
                        case 'v': verbose = 1; break;
                        default:
                            fprintf(stderr, "rmdir: invalid option -- '%c'\n", *c);
                            fprintf(stderr, "Try 'rmdir --help' for more information.\n");
                            free(ops);
                            return 1;
                    }
                }
            }
        } else {
            ops[nops++] = a;
        }
    }

    if (nops == 0) {
        fprintf(stderr, "rmdir: missing operand\n");
        fprintf(stderr, "Try 'rmdir --help' for more information.\n");
        free(ops);
        return 1;
    }

    for (i = 0; i < nops; i++) {
        char *dir = ops[i];
        if (verbose)
            printf("rmdir: removing directory, '%s'\n", dir);
        if (rmdir(dir) != 0) {
            if (ignore_ne && (errno == ENOTEMPTY || errno == EEXIST))
                continue;
            fprintf(stderr, "rmdir: failed to remove '%s': %s\n", dir, strerror(errno));
            ok = 0;
        } else if (parents) {
            if (!rmdir_parents_fn(dir, verbose, ignore_ne))
                ok = 0;
        }
    }

    free(ops);
    return ok ? 0 : 1;
}

/* ---- rm ---- */
/* Is the last path component "." or ".."? (trailing slashes ignored) */
static int rm_is_dot(const char *p) {
    size_t len = strlen(p);
    while (len > 1 && p[len-1] == '/') len--;       /* ignore trailing '/' */
    size_t start = len;
    while (start > 0 && p[start-1] != '/') start--; /* start of last comp */
    size_t clen = len - start;
    if (clen == 1 && p[start] == '.') return 1;
    if (clen == 2 && p[start] == '.' && p[start+1] == '.') return 1;
    return 0;
}

/* Remove one path. recursive/force control -r/-f. Returns 0 ok, 1 on error. */
static int rm_do(const char *path, int recursive, int force) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (force && errno == ENOENT) return 0;
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(EISDIR));
            return 1;
        }
        DIR *d = opendir(path);
        if (!d) {
            if (force && errno == ENOENT) return 0;
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
            return 1;
        }
        int fail = 0;
        size_t plen = strlen(path);
        int slash = (plen > 0 && path[plen-1] == '/');
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t need = plen + 1 + strlen(e->d_name) + 1;
            char *child = malloc(need);
            if (!child) { fprintf(stderr, "rm: memory exhausted\n"); fail = 1; continue; }
            if (slash) snprintf(child, need, "%s%s", path, e->d_name);
            else       snprintf(child, need, "%s/%s", path, e->d_name);
            if (rm_do(child, recursive, force) != 0) fail = 1;
            free(child);
        }
        closedir(d);
        if (fail) return 1;                          /* dir not emptied */
        if (rmdir(path) != 0) {
            if (force && errno == ENOENT) return 0;
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (unlink(path) != 0) {
        if (force && errno == ENOENT) return 0;
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int a_rm(int argc, char **argv) {
    int recursive = 0, force = 0, endopt = 0, status = 0, nfiles = 0;
    char **files = malloc((size_t)argc * sizeof *files);
    if (!files) { fprintf(stderr, "rm: memory exhausted\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopt && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-' && a[2] == '\0') { endopt = 1; continue; }  /* -- */
            if (a[1] == '-') {                                          /* long */
                if (!strcmp(a, "--recursive")) { recursive = 1; continue; }
                if (!strcmp(a, "--force"))     { force = 1; continue; }
                fprintf(stderr, "rm: unrecognized option '%s'\n", a);
                fprintf(stderr, "Try 'rm --help' for more information.\n");
                free(files); return 1;
            }
            for (char *p = a + 1; *p; p++) {                            /* short */
                if (*p == 'r' || *p == 'R') recursive = 1;
                else if (*p == 'f') force = 1;
                else {
                    fprintf(stderr, "rm: invalid option -- '%c'\n", *p);
                    fprintf(stderr, "Try 'rm --help' for more information.\n");
                    free(files); return 1;
                }
            }
            continue;
        }
        files[nfiles++] = a;
    }
    if (nfiles == 0) {
        if (!force) {
            fprintf(stderr, "rm: missing operand\n");
            fprintf(stderr, "Try 'rm --help' for more information.\n");
            status = 1;
        }
        free(files);
        return status;
    }
    for (int i = 0; i < nfiles; i++) {
        if (rm_is_dot(files[i])) {
            fprintf(stderr, "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", files[i]);
            status = 1;
            continue;
        }
        if (rm_do(files[i], recursive, force) != 0) status = 1;
    }
    free(files);
    return status;
}

/* ---- cp ---- */
static char *cp_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = (dl > 0 && dir[dl - 1] == '/') ? 0 : 1;
    char *p = malloc(dl + slash + nl + 1);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    memcpy(p + dl + slash, name, nl);
    p[dl + slash + nl] = '\0';
    return p;
}

static char *cp_basedup(const char *path) {
    size_t len = strlen(path), i, start = 0, blen;
    char *b;
    while (len > 1 && path[len - 1] == '/') len--;
    for (i = 0; i < len; i++)
        if (path[i] == '/') start = i + 1;
    blen = len - start;
    b = malloc(blen + 1);
    if (!b) return NULL;
    memcpy(b, path + start, blen);
    b[blen] = '\0';
    return b;
}

static int cp_copy_reg(const char *src, const char *dst, mode_t mode) {
    int in, out, rc = 0;
    char buf[65536];
    ssize_t n;
    in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s' for reading: %s\n", src, strerror(errno));
        return 1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create regular file '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0, w;
        while (off < n) {
            w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                fprintf(stderr, "cp: error writing '%s': %s\n", dst, strerror(errno));
                rc = 1;
                goto done;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: error reading '%s': %s\n", src, strerror(errno));
        rc = 1;
    }
done:
    (void)fchmod(out, mode & 0777);
    close(in);
    close(out);
    return rc;
}

static int cp_one(const char *src, const char *dst, int recursive);

static int cp_dir(const char *src, const char *dst, mode_t mode, int recursive) {
    struct stat dsst;
    DIR *d;
    struct dirent *de;
    int rc = 0;
    if (stat(dst, &dsst) == 0) {
        if (!S_ISDIR(dsst.st_mode)) {
            fprintf(stderr, "cp: cannot overwrite non-directory '%s' with directory '%s'\n", dst, src);
            return 1;
        }
    } else if (mkdir(dst, mode & 0777) != 0) {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno));
        return 1;
    }
    struct stat dstst;
    stat(dst, &dstst);                          /* for the copy-into-itself guard */
    d = opendir(src);
    if (!d) {
        fprintf(stderr, "cp: cannot open directory '%s': %s\n", src, strerror(errno));
        return 1;
    }
    while ((de = readdir(d)) != NULL) {
        char *sp, *dp;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        sp = cp_join(src, de->d_name);
        dp = cp_join(dst, de->d_name);
        if (!sp || !dp) {
            fprintf(stderr, "cp: memory exhausted\n");
            free(sp); free(dp);
            rc = 1;
            continue;
        }
        /* lstat the child so we don't follow symlinks during recursion (a symlink
         * loop like a/self -> . would recurse forever) and skip the destination
         * itself (cp -r a a/sub would otherwise copy into itself endlessly). */
        struct stat cst;
        if (lstat(sp, &cst) == 0) {
            if (S_ISDIR(cst.st_mode) && cst.st_dev == dstst.st_dev && cst.st_ino == dstst.st_ino) {
                free(sp); free(dp); continue;                 /* this child IS the dest dir */
            }
            if (S_ISLNK(cst.st_mode)) {                       /* copy symlink as a symlink */
                char t[4096]; ssize_t k = readlink(sp, t, sizeof t - 1);
                if (k >= 0) { t[k] = 0; unlink(dp); if (symlink(t, dp) != 0) rc = 1; }
                else rc = 1;
                free(sp); free(dp); continue;
            }
        }
        if (cp_one(sp, dp, recursive) != 0) rc = 1;
        free(sp);
        free(dp);
    }
    closedir(d);
    return rc;
}

static int cp_one(const char *src, const char *dst, int recursive) {
    struct stat st, dstat;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
            return 1;
        }
        return cp_dir(src, dst, st.st_mode, recursive);
    }
    if (stat(dst, &dstat) == 0 && dstat.st_dev == st.st_dev && dstat.st_ino == st.st_ino) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }
    return cp_copy_reg(src, dst, st.st_mode);
}

static int a_cp(int argc, char **argv) {
    int recursive = 0, endopts = 0, nops = 0, nsrc, i, rc = 0, dest_is_dir;
    char **ops, *dest;
    struct stat dst_st;

    ops = malloc(sizeof(char *) * (argc > 0 ? argc : 1));
    if (!ops) {
        fprintf(stderr, "cp: memory exhausted\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!endopts && arg[0] == '-' && arg[1] != '\0') {
            int j;
            if (strcmp(arg, "--") == 0) { endopts = 1; continue; }
            if (strcmp(arg, "--recursive") == 0) { recursive = 1; continue; }
            for (j = 1; arg[j]; j++) {
                if (arg[j] == 'r' || arg[j] == 'R') {
                    recursive = 1;
                } else {
                    fprintf(stderr, "cp: invalid option -- '%c'\n", arg[j]);
                    free(ops);
                    return 1;
                }
            }
        } else {
            ops[nops++] = arg;
        }
    }
    if (nops == 0) {
        fprintf(stderr, "cp: missing file operand\n");
        free(ops);
        return 1;
    }
    if (nops == 1) {
        fprintf(stderr, "cp: missing destination file operand after '%s'\n", ops[0]);
        free(ops);
        return 1;
    }
    dest = ops[nops - 1];
    nsrc = nops - 1;
    dest_is_dir = (stat(dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));

    if (dest_is_dir) {
        for (i = 0; i < nsrc; i++) {
            char *b = cp_basedup(ops[i]);
            char *target;
            if (!b) { fprintf(stderr, "cp: memory exhausted\n"); rc = 1; continue; }
            target = cp_join(dest, b);
            free(b);
            if (!target) { fprintf(stderr, "cp: memory exhausted\n"); rc = 1; continue; }
            if (cp_one(ops[i], target, recursive) != 0) rc = 1;
            free(target);
        }
    } else {
        if (nsrc > 1) {
            fprintf(stderr, "cp: target '%s' is not a directory\n", dest);
            free(ops);
            return 1;
        }
        if (cp_one(ops[0], dest, recursive) != 0) rc = 1;
    }
    free(ops);
    return rc;
}

/* ---- mv ---- */
static char *mv_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    int slash = (dl > 0 && dir[dl - 1] == '/') ? 0 : 1;
    char *p = malloc(dl + (size_t)slash + strlen(name) + 1);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    strcpy(p + dl + slash, name);
    return p;
}

static char *mv_base(const char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t start = 0, i;
    for (i = 0; i < len; i++)
        if (path[i] == '/') start = i + 1;
    size_t blen = len - start;
    char *b = malloc(blen + 1);
    if (!b) return NULL;
    memcpy(b, path + start, blen);
    b[blen] = '\0';
    return b;
}

static void mv_preserve_times(const char *path, struct stat *st) {
    struct utimbuf t;
    t.actime = st->st_atime;
    t.modtime = st->st_mtime;
    if (utime(path, &t)) {}
}

static int mv_copy_reg(const char *src, const char *dst, struct stat *st) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "mv: cannot open '%s' for reading: %s\n", src, strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777);
    if (out < 0) {
        fprintf(stderr, "mv: cannot create regular file '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                fprintf(stderr, "mv: error writing '%s': %s\n", dst, strerror(errno));
                rc = 1;
                break;
            }
            off += w;
        }
        if (rc) break;
    }
    if (n < 0) {
        fprintf(stderr, "mv: error reading '%s': %s\n", src, strerror(errno));
        rc = 1;
    }
    if (fchown(out, st->st_uid, st->st_gid)) {}
    if (fchmod(out, st->st_mode & 07777)) {}
    if (close(out) != 0) {
        fprintf(stderr, "mv: error closing '%s': %s\n", dst, strerror(errno));
        rc = 1;
    }
    close(in);
    if (rc == 0) mv_preserve_times(dst, st);
    return rc;
}

static int mv_copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        fprintf(stderr, "mv: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        mode_t dmode = (st.st_mode & 07777) | S_IRWXU;
        if (mkdir(dst, dmode) != 0 && errno != EEXIST) {
            fprintf(stderr, "mv: cannot create directory '%s': %s\n", dst, strerror(errno));
            return 1;
        }
        DIR *d = opendir(src);
        if (!d) {
            fprintf(stderr, "mv: cannot access '%s': %s\n", src, strerror(errno));
            return 1;
        }
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char *sp = mv_join(src, e->d_name);
            char *dp = mv_join(dst, e->d_name);
            if (!sp || !dp) { free(sp); free(dp); rc = 1; continue; }
            if (mv_copy_tree(sp, dp) != 0) rc = 1;
            free(sp);
            free(dp);
        }
        closedir(d);
        if (chown(dst, st.st_uid, st.st_gid)) {}
        if (chmod(dst, st.st_mode & 07777)) {}
        mv_preserve_times(dst, &st);
        return rc;
    } else if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(src, buf, sizeof(buf) - 1);
        if (n < 0) {
            fprintf(stderr, "mv: cannot read symbolic link '%s': %s\n", src, strerror(errno));
            return 1;
        }
        buf[n] = '\0';
        if (unlink(dst) != 0 && errno != ENOENT) {
            fprintf(stderr, "mv: cannot remove '%s': %s\n", dst, strerror(errno));
            return 1;
        }
        if (symlink(buf, dst) != 0) {
            fprintf(stderr, "mv: cannot create symbolic link '%s': %s\n", dst, strerror(errno));
            return 1;
        }
        if (lchown(dst, st.st_uid, st.st_gid)) {}
        return 0;
    } else if (S_ISREG(st.st_mode)) {
        return mv_copy_reg(src, dst, &st);
    } else {
        if (unlink(dst) != 0 && errno != ENOENT) {
            fprintf(stderr, "mv: cannot remove '%s': %s\n", dst, strerror(errno));
            return 1;
        }
        if (mknod(dst, st.st_mode, st.st_rdev) != 0) {
            fprintf(stderr, "mv: cannot create special file '%s': %s\n", dst, strerror(errno));
            return 1;
        }
        if (chown(dst, st.st_uid, st.st_gid)) {}
        if (chmod(dst, st.st_mode & 07777)) {}
        mv_preserve_times(dst, &st);
        return 0;
    }
}

static int mv_rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "mv: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "mv: cannot access '%s': %s\n", path, strerror(errno));
            return 1;
        }
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char *p = mv_join(path, e->d_name);
            if (!p) { rc = 1; continue; }
            if (mv_rm_tree(p) != 0) rc = 1;
            free(p);
        }
        closedir(d);
        if (rmdir(path) != 0) {
            fprintf(stderr, "mv: cannot remove '%s': %s\n", path, strerror(errno));
            rc = 1;
        }
        return rc;
    }
    if (unlink(path) != 0) {
        fprintf(stderr, "mv: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int mv_one(const char *src, const char *dst,
                  int interactive, int noclobber, int verbose) {
    struct stat ss, ds;
    if (lstat(src, &ss) != 0) {
        fprintf(stderr, "mv: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    int have_ds = (lstat(dst, &ds) == 0);
    if (have_ds) {
        if (noclobber) return 0;
        if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino) {
            fprintf(stderr, "mv: '%s' and '%s' are the same file\n", src, dst);
            return 1;
        }
        if (interactive) {
            int c, first;
            fprintf(stderr, "mv: overwrite '%s'? ", dst);
            c = getchar();
            first = c;
            while (c != '\n' && c != EOF) c = getchar();
            if (first != 'y' && first != 'Y') return 0;
        }
    }
    if (rename(src, dst) == 0) {
        if (verbose) printf("renamed '%s' -> '%s'\n", src, dst);
        return 0;
    }
    if (errno == EXDEV) {
        if (mv_copy_tree(src, dst) != 0) return 1;
        if (mv_rm_tree(src) != 0) return 1;
        if (verbose) printf("renamed '%s' -> '%s'\n", src, dst);
        return 0;
    }
    fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", src, dst, strerror(errno));
    return 1;
}

static int mv_into_dir(const char *dir, char **srcs, int n,
                       int interactive, int noclobber, int verbose) {
    int rc = 0, i;
    for (i = 0; i < n; i++) {
        char *base = mv_base(srcs[i]);
        char *dst = base ? mv_join(dir, base) : NULL;
        if (!base || !dst) {
            fprintf(stderr, "mv: memory exhausted\n");
            free(base);
            free(dst);
            rc = 1;
            continue;
        }
        if (mv_one(srcs[i], dst, interactive, noclobber, verbose) != 0) rc = 1;
        free(base);
        free(dst);
    }
    return rc;
}

static int a_mv(int argc, char **argv) {
    int interactive = 0, noclobber = 0, verbose = 0, no_target_dir = 0;
    char *target_dir = NULL;
    char **ops = malloc(sizeof(char *) * (size_t)(argc > 0 ? argc : 1));
    int nops = 0, end_opts = 0, rc = 0, i;
    if (!ops) {
        fprintf(stderr, "mv: memory exhausted\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!end_opts && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-' && a[2] == '\0') { end_opts = 1; continue; }
            if (a[1] == '-') {
                if (!strcmp(a, "--force")) { interactive = 0; noclobber = 0; }
                else if (!strcmp(a, "--interactive")) { interactive = 1; noclobber = 0; }
                else if (!strcmp(a, "--no-clobber")) { noclobber = 1; interactive = 0; }
                else if (!strcmp(a, "--verbose")) verbose = 1;
                else if (!strcmp(a, "--no-target-directory")) no_target_dir = 1;
                else if (!strncmp(a, "--target-directory=", 19)) target_dir = a + 19;
                else if (!strcmp(a, "--target-directory")) {
                    if (i + 1 < argc) target_dir = argv[++i];
                    else {
                        fprintf(stderr, "mv: option '--target-directory' requires an argument\n");
                        rc = 1;
                        goto done;
                    }
                } else {
                    fprintf(stderr, "mv: unrecognized option '%s'\n", a);
                    rc = 1;
                    goto done;
                }
            } else {
                int j = 1;
                while (a[j]) {
                    char c = a[j];
                    if (c == 'f') { interactive = 0; noclobber = 0; }
                    else if (c == 'i') { interactive = 1; noclobber = 0; }
                    else if (c == 'n') { noclobber = 1; interactive = 0; }
                    else if (c == 'v') verbose = 1;
                    else if (c == 'T') no_target_dir = 1;
                    else if (c == 't') {
                        if (a[j + 1]) target_dir = &a[j + 1];
                        else if (i + 1 < argc) target_dir = argv[++i];
                        else {
                            fprintf(stderr, "mv: option requires an argument -- 't'\n");
                            rc = 1;
                            goto done;
                        }
                        break;
                    } else {
                        fprintf(stderr, "mv: invalid option -- '%c'\n", c);
                        rc = 1;
                        goto done;
                    }
                    j++;
                }
            }
        } else {
            ops[nops++] = a;
        }
    }

    if (target_dir) {
        if (nops == 0) {
            fprintf(stderr, "mv: missing file operand\n");
            rc = 1;
            goto done;
        }
        rc = mv_into_dir(target_dir, ops, nops, interactive, noclobber, verbose);
    } else {
        if (nops == 0) {
            fprintf(stderr, "mv: missing file operand\n");
            rc = 1;
            goto done;
        }
        if (nops == 1) {
            fprintf(stderr, "mv: missing destination file operand after '%s'\n", ops[0]);
            rc = 1;
            goto done;
        }
        char *dest = ops[nops - 1];
        if (no_target_dir) {
            if (nops > 2) {
                fprintf(stderr, "mv: extra operand '%s'\n", ops[2]);
                rc = 1;
                goto done;
            }
            rc = mv_one(ops[0], dest, interactive, noclobber, verbose);
        } else {
            struct stat dst_st;
            int dest_is_dir = (stat(dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));
            if (dest_is_dir) {
                rc = mv_into_dir(dest, ops, nops - 1, interactive, noclobber, verbose);
            } else {
                if (nops > 2) {
                    fprintf(stderr, "mv: target '%s' is not a directory\n", dest);
                    rc = 1;
                    goto done;
                }
                rc = mv_one(ops[0], dest, interactive, noclobber, verbose);
            }
        }
    }
done:
    free(ops);
    return rc;
}

/* ---- ln ---- */
/* ---- basename of TARGET (malloc'd), stripping trailing slashes ---- */
static char *ln_base(const char *p) {
    size_t len = strlen(p);
    while (len > 1 && p[len-1] == '/') len--;
    size_t s = len;
    while (s > 0 && p[s-1] != '/') s--;
    char *out = malloc(len - s + 1);
    if (out) { memcpy(out, p + s, len - s); out[len - s] = '\0'; }
    return out;
}

/* ---- join DIR "/" BASE into a malloc'd path, avoiding a double slash ---- */
static char *ln_join(const char *dir, const char *base) {
    size_t dl = strlen(dir);
    int slash = (dl > 0 && dir[dl-1] == '/') ? 0 : 1;
    char *out = malloc(dl + slash + strlen(base) + 1);
    if (!out) return NULL;
    memcpy(out, dir, dl);
    if (slash) out[dl] = '/';
    strcpy(out + dl + slash, base);
    return out;
}

/* ---- create one link src -> dest; force removes an existing dest first ---- */
static int ln_make(const char *src, const char *dest, int sym, int force) {
    int r = sym ? symlink(src, dest) : link(src, dest);
    if (r != 0 && force && errno == EEXIST) {
        struct stat a, b;
        if (stat(src, &a) == 0 && lstat(dest, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
            fprintf(stderr, "ln: '%s' and '%s' are the same file\n", src, dest);
            return 1;
        }
        if (unlink(dest) == 0)
            r = sym ? symlink(src, dest) : link(src, dest);
    }
    if (r != 0) {
        if (sym)
            fprintf(stderr, "ln: failed to create symbolic link '%s': %s\n",
                    dest, strerror(errno));
        else
            fprintf(stderr, "ln: failed to create hard link '%s' => '%s': %s\n",
                    dest, src, strerror(errno));
        return 1;
    }
    return 0;
}

/* ln [-sf] TARGET LINKNAME  |  ln [-sf] TARGET... DIR */
static int a_ln(int argc, char **argv) {
    int sym = 0, force = 0, i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;          /* operand or "-" */
        if (a[1] == '-') {                                /* "--" or long opt */
            if (a[2] == '\0') { i++; break; }
            if (!strcmp(a, "--symbolic")) sym = 1;
            else if (!strcmp(a, "--force")) force = 1;
            else { fprintf(stderr, "ln: unrecognized option '%s'\n", a); return 1; }
            continue;
        }
        for (int j = 1; a[j]; j++) {
            if (a[j] == 's') sym = 1;
            else if (a[j] == 'f') force = 1;
            else { fprintf(stderr, "ln: invalid option -- '%c'\n", a[j]); return 1; }
        }
    }

    int n = argc - i;
    char **op = argv + i;
    if (n == 0) {
        fprintf(stderr, "ln: missing file operand\n");
        return 1;
    }

    if (n == 1) {                                         /* ln TARGET */
        char *base = ln_base(op[0]);
        if (!base) { fprintf(stderr, "ln: memory exhausted\n"); return 1; }
        int rc = ln_make(op[0], base, sym, force);
        free(base);
        return rc;
    }

    /* n >= 2: is the last operand a directory? */
    struct stat st;
    int lastdir = (stat(op[n-1], &st) == 0 && S_ISDIR(st.st_mode));

    if (!lastdir) {
        if (n > 2) {
            fprintf(stderr, "ln: target '%s' is not a directory\n", op[n-1]);
            return 1;
        }
        return ln_make(op[0], op[1], sym, force);         /* ln TARGET LINKNAME */
    }

    /* ln TARGET... DIR */
    const char *dir = op[n-1];
    int rc = 0;
    for (int k = 0; k < n - 1; k++) {
        char *base = ln_base(op[k]);
        char *dest = base ? ln_join(dir, base) : NULL;
        if (!dest) { fprintf(stderr, "ln: memory exhausted\n"); free(base); rc = 1; continue; }
        rc |= ln_make(op[k], dest, sym, force);
        free(base);
        free(dest);
    }
    return rc;
}

/* ---- touch ---- */
static int a_touch(int argc, char **argv)
{
    int no_create = 0, a_flag = 0, m_flag = 0;
    int i, status = 0, end_opts = 0, nfiles = 0;
    struct timespec ts[2];

    /* Pass 1: collect options (getopt-style: options may follow operands). */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (end_opts)
            continue;
        if (strcmp(arg, "--") == 0) { end_opts = 1; continue; }
        if (arg[0] == '-' && arg[1] != '\0') {
            char *p = arg + 1;
            while (*p) {
                if (*p == 'c')      no_create = 1;
                else if (*p == 'a') a_flag = 1;
                else if (*p == 'm') m_flag = 1;
                else if (*p == 't' || *p == 'd' || *p == 'r') {
                    /* option takes an argument (ignored); consume it */
                    if (p[1] == '\0')
                        i++;
                    break;
                }
                /* other options are ignored */
                p++;
            }
        }
    }

    if (!a_flag && !m_flag) { a_flag = 1; m_flag = 1; }
    ts[0].tv_sec = 0; ts[0].tv_nsec = a_flag ? UTIME_NOW : UTIME_OMIT;
    ts[1].tv_sec = 0; ts[1].tv_nsec = m_flag ? UTIME_NOW : UTIME_OMIT;

    /* Pass 2: process file operands. */
    end_opts = 0;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (!end_opts) {
            if (strcmp(arg, "--") == 0) { end_opts = 1; continue; }
            if (arg[0] == '-' && arg[1] != '\0') {
                char *p = arg + 1;
                while (*p) {
                    if (*p == 't' || *p == 'd' || *p == 'r') {
                        if (p[1] == '\0')
                            i++;
                        break;
                    }
                    p++;
                }
                continue;
            }
        }

        nfiles++;

        if (utimensat(AT_FDCWD, arg, ts, 0) == 0)
            continue;

        if (errno == ENOENT) {
            if (no_create)
                continue;               /* -c: succeed silently, do not create */
            int fd = open(arg, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
            if (fd < 0) {
                fprintf(stderr, "%s: cannot touch '%s': %s\n",
                        argv[0], arg, strerror(errno));
                status = 1;
                continue;
            }
            close(fd);
            if (utimensat(AT_FDCWD, arg, ts, 0) != 0) {
                fprintf(stderr, "%s: setting times of '%s': %s\n",
                        argv[0], arg, strerror(errno));
                status = 1;
            }
        } else {
            fprintf(stderr, "%s: cannot touch '%s': %s\n",
                    argv[0], arg, strerror(errno));
            status = 1;
        }
    }

    if (nfiles == 0) {
        fprintf(stderr, "%s: missing file operand\n", argv[0]);
        return 1;
    }

    return status;
}

/* ------------------------------------------------- extended applets 2 --- */
/* ---- sort ---- */
/* sort — sort lines of text. Flags: -n -r -u -f -b (no -k). C locale, byte-wise
 * default. Matches GNU: numeric compares the leading number (no exponent) at
 * arbitrary precision; when keys tie, whole lines are compared byte-wise as a
 * last resort (only -r reverses that); -u drops lines whose *key* compares
 * equal to the previous line; a final line lacking a newline still counts and
 * every output line is newline-terminated. */
static int sort_nflag, sort_rflag, sort_fflag, sort_bflag;

/* true if the numeric prefix at p has zero magnitude ("0", "0.0", "", "abc"...) */
static int sort_iszero(const char *p) {
    for (; (*p >= '0' && *p <= '9') || *p == '.'; p++)
        if (*p > '0' && *p <= '9') return 0;
    return 1;
}

/* compare magnitudes of two numeric prefixes (sign already stripped) */
static int sort_magcmp(const char *a, const char *b) {
    const char *ai = a, *bi = b;
    while (*ai == '0') ai++;
    while (*bi == '0') bi++;
    const char *ae = ai; while (*ae >= '0' && *ae <= '9') ae++;
    const char *be = bi; while (*be >= '0' && *be <= '9') be++;
    long la = ae - ai, lb = be - bi;
    if (la != lb) return la < lb ? -1 : 1;
    while (ai < ae) {
        if (*ai != *bi) return (unsigned char)*ai < (unsigned char)*bi ? -1 : 1;
        ai++; bi++;
    }
    const char *af = ae, *bf = be;
    if (*af == '.') af++;
    if (*bf == '.') bf++;
    for (;;) {
        int ca = (*af >= '0' && *af <= '9');
        int cb = (*bf >= '0' && *bf <= '9');
        if (!ca && !cb) return 0;
        int da = ca ? (unsigned char)*af : '0';
        int db = cb ? (unsigned char)*bf : '0';
        if (da != db) return da < db ? -1 : 1;
        if (ca) af++;
        if (cb) bf++;
    }
}

/* GNU-style leading-number comparison (leading blanks skipped, no exponent) */
static int sort_numcmp(const char *a, const char *b) {
    while (*a == ' ' || *a == '\t') a++;
    while (*b == ' ' || *b == '\t') b++;
    int sa = 1, sb = 1;
    if (*a == '-') { sa = -1; a++; }
    if (*b == '-') { sb = -1; b++; }
    int ea = sort_iszero(a) ? 0 : sa;   /* -0 and non-numeric collapse to 0 */
    int eb = sort_iszero(b) ? 0 : sb;
    if (ea != eb) return ea < eb ? -1 : 1;
    int mag = sort_magcmp(a, b);
    return ea < 0 ? -mag : mag;
}

/* case-folding byte comparison (C locale toupper) */
static int sort_foldcmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        int fa = toupper(ca), fb = toupper(cb);
        if (fa != fb) return fa < fb ? -1 : 1;
        if (ca == 0) return 0;
    }
}

/* key comparison honouring -n/-f/-b; no last-resort, no reverse */
static int sort_keycmp(const char *a, const char *b) {
    const char *pa = a, *pb = b;
    if (sort_bflag) {
        while (*pa == ' ' || *pa == '\t') pa++;
        while (*pb == ' ' || *pb == '\t') pb++;
    }
    if (sort_nflag) return sort_numcmp(pa, pb);
    if (sort_fflag) return sort_foldcmp(pa, pb);
    return strcmp(pa, pb);
}

static int sort_cmp(const void *pa, const void *pb) {
    const char *a = *(const char *const *)pa;
    const char *b = *(const char *const *)pb;
    int r = sort_keycmp(a, b);
    if (r == 0) r = strcmp(a, b);      /* last resort: whole line, byte-wise */
    return sort_rflag ? -r : r;
}

/* read one line (newline stripped); returns NULL at EOF with nothing read */
static char *sort_readline(FILE *fp) {
    size_t cap = 128, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = getc(fp)) != EOF) {
        if (len + 1 >= cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (c == '\n') { buf[len] = '\0'; return buf; }
        buf[len++] = (char)c;
    }
    if (len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

static int a_sort(int argc, char **argv) {
    sort_nflag = sort_rflag = sort_fflag = sort_bflag = 0;
    int uflag = 0, endopts = 0, i;
    char **files = malloc((argc > 0 ? argc : 1) * sizeof(char *));
    int nfiles = 0;
    if (!files) { fprintf(stderr, "sort: out of memory\n"); return 2; }

    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!endopts && arg[0] == '-' && arg[1] != '\0') {
            if (arg[1] == '-' && arg[2] == '\0') { endopts = 1; continue; }
            for (char *p = arg + 1; *p; p++) {
                switch (*p) {
                    case 'n': sort_nflag = 1; break;
                    case 'r': sort_rflag = 1; break;
                    case 'u': uflag = 1; break;
                    case 'f': sort_fflag = 1; break;
                    case 'b': sort_bflag = 1; break;
                    default:
                        fprintf(stderr, "sort: invalid option -- '%c'\n", *p);
                        free(files);
                        return 2;
                }
            }
        } else {
            files[nfiles++] = arg;
        }
    }

    char *stdin_src = "-";
    char **srcs = files; int nsrc = nfiles;
    if (nsrc == 0) { srcs = &stdin_src; nsrc = 1; }

    char **lines = NULL;
    size_t nlines = 0, cap = 0;

    for (i = 0; i < nsrc; i++) {
        FILE *fp;
        if (strcmp(srcs[i], "-") == 0) fp = stdin;
        else {
            fp = fopen(srcs[i], "r");
            if (!fp) {
                fprintf(stderr, "sort: %s: %s\n", srcs[i], strerror(errno));
                for (size_t k = 0; k < nlines; k++) free(lines[k]);
                free(lines); free(files);
                return 2;
            }
        }
        char *ln;
        while ((ln = sort_readline(fp)) != NULL) {
            if (nlines >= cap) {
                size_t ncap = cap ? cap * 2 : 128;
                char **nl = realloc(lines, ncap * sizeof(char *));
                if (!nl) {
                    free(ln);
                    for (size_t k = 0; k < nlines; k++) free(lines[k]);
                    free(lines); free(files);
                    if (fp != stdin) fclose(fp);
                    fprintf(stderr, "sort: out of memory\n");
                    return 2;
                }
                lines = nl; cap = ncap;
            }
            lines[nlines++] = ln;
        }
        if (fp != stdin) fclose(fp);
    }

    if (nlines > 1)
        qsort(lines, nlines, sizeof(char *), sort_cmp);

    for (size_t j = 0; j < nlines; j++) {
        if (uflag && j > 0 && sort_keycmp(lines[j - 1], lines[j]) == 0)
            continue;
        fputs(lines[j], stdout);
        putchar('\n');
    }

    for (size_t j = 0; j < nlines; j++) free(lines[j]);
    free(lines); free(files);
    return 0;
}

/* ---- tr ---- */
/* ---- tr ---- */
/* Character-class membership for tr set expansion (C locale, ASCII order). */
static int tr_class_valid(const char *name)
{
    static const char *n[] = { "alpha","digit","alnum","upper","lower","space",
                               "blank","punct","cntrl","graph","print","xdigit", 0 };
    for (int i = 0; n[i]; i++)
        if (!strcmp(name, n[i])) return 1;
    return 0;
}

static int tr_class_has(const char *name, int c)
{
    unsigned char u = (unsigned char)c;
    if (!strcmp(name, "alpha"))  return isalpha(u)  != 0;
    if (!strcmp(name, "digit"))  return isdigit(u)  != 0;
    if (!strcmp(name, "alnum"))  return isalnum(u)  != 0;
    if (!strcmp(name, "upper"))  return isupper(u)  != 0;
    if (!strcmp(name, "lower"))  return islower(u)  != 0;
    if (!strcmp(name, "space"))  return isspace(u)  != 0;
    if (!strcmp(name, "blank"))  return isblank(u)  != 0;
    if (!strcmp(name, "punct"))  return ispunct(u)  != 0;
    if (!strcmp(name, "cntrl"))  return iscntrl(u)  != 0;
    if (!strcmp(name, "graph"))  return isgraph(u)  != 0;
    if (!strcmp(name, "print"))  return isprint(u)  != 0;
    if (!strcmp(name, "xdigit")) return isxdigit(u) != 0;
    return 0;
}

/* Append one byte to a growable buffer; returns -1 on OOM. */
static int tr_push(unsigned char **buf, size_t *len, size_t *cap, int b)
{
    if (*len == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        unsigned char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    (*buf)[(*len)++] = (unsigned char)b;
    return 0;
}

/* Read one character unit (handling backslash escapes) from *pp, advancing it. */
static int tr_getone(const char **pp)
{
    const char *p = *pp;
    int c = (unsigned char)*p++;
    if (c == '\\' && *p) {
        int e = (unsigned char)*p;
        switch (e) {
        case '\\': c = '\\'; p++; break;
        case 'a':  c = '\a'; p++; break;
        case 'b':  c = '\b'; p++; break;
        case 'f':  c = '\f'; p++; break;
        case 'n':  c = '\n'; p++; break;
        case 'r':  c = '\r'; p++; break;
        case 't':  c = '\t'; p++; break;
        case 'v':  c = '\v'; p++; break;
        default:
            if (e >= '0' && e <= '7') {          /* octal escape, up to 3 digits */
                int val = 0, k = 0;
                while (k < 3 && *p >= '0' && *p <= '7') { val = val * 8 + (*p - '0'); p++; k++; }
                c = val & 0xFF;
            } else {                              /* unknown escape: literal char */
                c = e; p++;
            }
            break;
        }
    }
    *pp = p;
    return c;
}

/* Expand a tr SET string into an ordered byte list. Returns 0 / -1 (msg printed). */
static int tr_expand(const char *set, unsigned char **buf, size_t *len, size_t *cap)
{
    const char *p = set;
    while (*p) {
        if (p[0] == '[' && p[1] == ':') {         /* [:class:] */
            const char *e = strstr(p + 2, ":]");
            if (e) {
                size_t nl = (size_t)(e - (p + 2));
                char name[16];
                if (nl < sizeof name) {
                    memcpy(name, p + 2, nl); name[nl] = '\0';
                    if (tr_class_valid(name)) {
                        for (int c = 0; c < 256; c++)
                            if (tr_class_has(name, c))
                                if (tr_push(buf, len, cap, c)) goto oom;
                        p = e + 2;
                        continue;
                    }
                    fprintf(stderr, "tr: invalid character class '%s'\n", name);
                    return -1;
                }
            }
            /* not a well-formed class: fall through, treat '[' literally */
        }
        {
            int c1 = tr_getone(&p);
            if (*p == '-' && p[1] != '\0') {      /* range c1-c2 */
                const char *q = p + 1;
                int c2 = tr_getone(&q);
                p = q;
                if (c2 < c1) {
                    fprintf(stderr, "tr: range-endpoints of '%c-%c' are in reverse "
                                    "collating sequence order\n", c1, c2);
                    return -1;
                }
                for (int c = c1; c <= c2; c++)
                    if (tr_push(buf, len, cap, c)) goto oom;
            } else {
                if (tr_push(buf, len, cap, c1)) goto oom;
            }
        }
    }
    return 0;
oom:
    fprintf(stderr, "tr: memory exhausted\n");
    return -1;
}

static int a_tr(int argc, char **argv)
{
    int dflag = 0, sflag = 0, cflag = 0;
    char *set1 = NULL, *set2 = NULL;
    int nops = 0, endopts = 0;

    /* Single pass: like GNU getopt, options and operands may be intermixed. */
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopts && a[0] == '-' && a[1] != '\0') {
            if (!strcmp(a, "--")) { endopts = 1; continue; }
            if (a[1] == '-') {
                if      (!strcmp(a, "--delete"))          dflag = 1;
                else if (!strcmp(a, "--squeeze-repeats")) sflag = 1;
                else if (!strcmp(a, "--complement"))      cflag = 1;
                else { fprintf(stderr, "tr: unrecognized option '%s'\n", a); return 1; }
                continue;
            }
            for (char *q = a + 1; *q; q++) {
                switch (*q) {
                case 'd': dflag = 1; break;
                case 's': sflag = 1; break;
                case 'c': case 'C': cflag = 1; break;
                default:
                    fprintf(stderr, "tr: invalid option -- '%c'\n", *q);
                    return 1;
                }
            }
            continue;
        }
        if      (nops == 0) set1 = a;
        else if (nops == 1) set2 = a;
        else { fprintf(stderr, "tr: extra operand '%s'\n", a); return 1; }
        nops++;
    }

    if (nops == 0) {
        fprintf(stderr, "tr: missing operand\n");
        return 1;
    }
    if (dflag) {
        if (!sflag && nops > 1) {
            fprintf(stderr, "tr: extra operand '%s'\n"
                    "Only one string may be given when deleting without "
                    "squeezing repeats.\n", set2);
            return 1;
        }
    } else if (!sflag && nops < 2) {
        fprintf(stderr, "tr: missing operand after '%s'\n"
                "Two strings must be given when translating.\n", set1);
        return 1;
    }

    unsigned char *s1 = NULL, *s2 = NULL;
    size_t n1 = 0, n2 = 0, c1cap = 0, c2cap = 0;
    if (tr_expand(set1, &s1, &n1, &c1cap)) { free(s1); return 1; }
    if (set2 && tr_expand(set2, &s2, &n2, &c2cap)) { free(s1); free(s2); return 1; }

    int mem1[256] = {0};
    for (size_t k = 0; k < n1; k++) mem1[s1[k]] = 1;

    int do_translate = (!dflag && nops == 2);
    int do_delete    = dflag;
    int do_squeeze   = sflag;

    if (do_translate && n2 == 0) {
        fprintf(stderr, "tr: when not truncating set1, string2 must be non-empty\n");
        free(s1); free(s2);
        return 1;
    }

    unsigned char trans[256];
    for (int b = 0; b < 256; b++) trans[b] = (unsigned char)b;
    int del[256] = {0}, sq[256] = {0};

    if (do_translate) {
        unsigned char last2 = s2[n2 - 1];
        if (!cflag) {
            for (size_t k = 0; k < n1; k++)
                trans[s1[k]] = (k < n2) ? s2[k] : last2;
        } else {
            size_t k = 0;
            for (int b = 0; b < 256; b++)
                if (!mem1[b]) { trans[b] = (k < n2) ? s2[k] : last2; k++; }
        }
    }
    if (do_delete)
        for (int b = 0; b < 256; b++) del[b] = cflag ? !mem1[b] : mem1[b];
    if (do_squeeze) {
        if (nops == 2)
            for (size_t k = 0; k < n2; k++) sq[s2[k]] = 1;   /* squeeze on SET2 */
        else if (!dflag)
            for (int b = 0; b < 256; b++) sq[b] = cflag ? !mem1[b] : mem1[b];
        /* dflag && nops==1: squeeze set stays empty */
    }

    free(s1); free(s2);

    int rc = 0, last = -1;
    unsigned char inbuf[65536], outbuf[65536];
    size_t op = 0, rd;
    while ((rd = fread(inbuf, 1, sizeof inbuf, stdin)) > 0) {
        for (size_t k = 0; k < rd; k++) {
            int c = inbuf[k];
            if (do_delete && del[c]) continue;
            int oc = do_translate ? trans[c] : c;
            if (do_squeeze && sq[oc] && oc == last) continue;
            outbuf[op++] = (unsigned char)oc;
            last = oc;
            if (op == sizeof outbuf) {
                if (fwrite(outbuf, 1, op, stdout) != op) {
                    fprintf(stderr, "tr: write error: %s\n", strerror(errno));
                    return 1;
                }
                op = 0;
            }
        }
    }
    if (ferror(stdin)) {
        fprintf(stderr, "tr: read error: %s\n", strerror(errno));
        rc = 1;
    }
    if (op && fwrite(outbuf, 1, op, stdout) != op) {
        fprintf(stderr, "tr: write error: %s\n", strerror(errno));
        rc = 1;
    }
    if (fflush(stdout) != 0) {
        fprintf(stderr, "tr: write error: %s\n", strerror(errno));
        rc = 1;
    }
    return rc;
}

/* ---- nl ---- */
/* number lines of a stream. body: 'a' number all, 't' number non-empty
 * (GNU default), 'n' number none. *np is the shared running line counter. */
static void nl_run(FILE *f, char body, long *np, char **buf, size_t *bufsz) {
    ssize_t len;
    while ((len = getline(buf, bufsz, f)) != -1) {
        int numbered;
        if (body == 'a')      numbered = 1;            /* number every line   */
        else if (body == 't') numbered = (len > 1);    /* non-empty only      */
        else                  numbered = 0;            /* body == 'n': none   */
        if (numbered) printf("%6ld\t", ++(*np));       /* right-just w6 + TAB */
        else          printf("%6s\t", "");             /* 6 spaces + TAB      */
        fwrite(*buf, 1, (size_t)len, stdout);          /* line incl. its '\n' */
    }
}

static int a_nl(int argc, char **argv) {
    char body = 't';                                   /* GNU default -b t    */
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;        /* filename or "-"     */
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[1] == 'b') {                             /* -bSTYLE or -b STYLE */
            const char *style = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!style) {
                fprintf(stderr, "nl: option requires an argument -- 'b'\n");
                return 1;
            }
            if (style[0] == 'a' || style[0] == 't' || style[0] == 'n')
                body = style[0];
            else {
                fprintf(stderr, "nl: invalid body numbering style: '%s'\n", style);
                return 1;
            }
        } else {
            fprintf(stderr, "nl: invalid option -- '%c'\n", a[1]);
            return 1;
        }
    }

    long n = 0;                                        /* continuous counter  */
    char *buf = NULL; size_t bufsz = 0;
    int rc = 0;
    if (i >= argc) {
        nl_run(stdin, body, &n, &buf, &bufsz);         /* no files: stdin     */
    } else {
        for (; i < argc; i++) {
            FILE *f;
            if (strcmp(argv[i], "-") == 0) f = stdin;
            else {
                f = fopen(argv[i], "rb");
                if (!f) {
                    fprintf(stderr, "nl: %s: %s\n", argv[i], strerror(errno));
                    rc = 1;
                    continue;
                }
            }
            nl_run(f, body, &n, &buf, &bufsz);
            if (f != stdin) fclose(f);
        }
    }
    free(buf);
    return rc;
}

/* ---- tac ---- */
/* tac — concatenate and print files in reverse (last line first).
 * Records are '\n'-terminated (trailing separator, GNU default): each record
 * includes its terminating newline; text after the final newline is a partial
 * record printed first. Each file operand is reversed independently (matching
 * GNU tac), '-' means stdin, none means stdin. */

/* Append the whole of stream f to *buf (grown as needed). Returns 0, -1 on I/O error. */
static int tac_slurp_fd(FILE *f, char **buf, size_t *len, size_t *cap) {
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (*len + n > *cap) {
            size_t nc = *cap ? *cap : 65536;
            while (nc < *len + n) nc *= 2;
            char *nb = realloc(*buf, nc);
            if (!nb) return -1;
            *buf = nb; *cap = nc;
        }
        memcpy(*buf + *len, tmp, n);
        *len += n;
    }
    return ferror(f) ? -1 : 0;
}

/* Emit buf[0..len) with '\n'-terminated records in reverse order. */
static void tac_emit(const char *buf, size_t len) {
    size_t p = len;
    while (p > 0) {
        size_t start = 0;                       /* find last '\n' in [0, p-1) */
        for (size_t m = p - 1; m-- > 0; )
            if (buf[m] == '\n') { start = m + 1; break; }
        fwrite(buf + start, 1, p - start, stdout);
        p = start;
    }
}

static int a_tac(int argc, char **argv) {
    int rc = 0, files_seen = 0, endopts = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!endopts && !strcmp(a, "--")) { endopts = 1; continue; }
        files_seen = 1;
        int is_stdin = !strcmp(a, "-");
        FILE *f;
        if (is_stdin) f = stdin;
        else {
            f = fopen(a, "rb");
            if (!f) { fprintf(stderr, "tac: failed to open '%s' for reading: %s\n", a, strerror(errno)); rc = 1; continue; }
        }
        char *buf = NULL; size_t len = 0, cap = 0;
        if (tac_slurp_fd(f, &buf, &len, &cap) != 0) {
            fprintf(stderr, "tac: %s: %s\n", is_stdin ? "standard input" : a, strerror(errno));
            rc = 1;
        } else {
            tac_emit(buf, len);
        }
        free(buf);
        if (!is_stdin) fclose(f);
    }
    if (!files_seen) {
        char *buf = NULL; size_t len = 0, cap = 0;
        if (tac_slurp_fd(stdin, &buf, &len, &cap) != 0) {
            fprintf(stderr, "tac: standard input: %s\n", strerror(errno));
            rc = 1;
        } else {
            tac_emit(buf, len);
        }
        free(buf);
    }
    return rc;
}

/* ---- tee ---- */
static int a_tee(int c, char **v) {
    int append = 0, i = 1;
    for (; i < c; i++) {
        if (v[i][0] != '-' || !v[i][1]) break;      /* operand, or "-" = file named "-" */
        if (!strcmp(v[i], "--")) { i++; break; }
        if (v[i][1] == '-') {
            char *o = v[i] + 2;
            if (!strcmp(o, "append")) append = 1;
            else { fprintf(stderr, "tee: unrecognized option '%s'\n", v[i]); return 1; }
            continue;
        }
        for (char *p = v[i] + 1; *p; p++) switch (*p) {
            case 'a': append = 1; break;
            default: fprintf(stderr, "tee: invalid option -- '%c'\n", *p); return 1;
        }
    }
    int nfiles = c - i, rc = 0;
    FILE **out = NULL;
    if (nfiles > 0) {
        out = malloc(sizeof(FILE *) * (size_t)nfiles);
        if (!out) { fprintf(stderr, "tee: memory exhausted\n"); return 1; }
    }
    const char *mode = append ? "ab" : "wb";
    for (int k = 0; k < nfiles; k++) {
        out[k] = fopen(v[i + k], mode);
        if (!out[k]) { fprintf(stderr, "tee: %s: %s\n", v[i + k], strerror(errno)); rc = 1; }
    }
    char buf[65536]; size_t n; int stdout_ok = 1;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) {
        if (stdout_ok && fwrite(buf, 1, n, stdout) != n) {
            fprintf(stderr, "tee: standard output: %s\n", strerror(errno));
            stdout_ok = 0; rc = 1;
        }
        for (int k = 0; k < nfiles; k++) {
            if (out[k] && fwrite(buf, 1, n, out[k]) != n) {
                fprintf(stderr, "tee: %s: %s\n", v[i + k], strerror(errno));
                fclose(out[k]); out[k] = NULL; rc = 1;
            }
        }
    }
    if (ferror(stdin)) { fprintf(stderr, "tee: standard input: %s\n", strerror(errno)); rc = 1; }
    if (stdout_ok && fflush(stdout) != 0) {
        fprintf(stderr, "tee: standard output: %s\n", strerror(errno)); rc = 1;
    }
    for (int k = 0; k < nfiles; k++)
        if (out[k] && fclose(out[k]) != 0) {
            fprintf(stderr, "tee: %s: %s\n", v[i + k], strerror(errno)); rc = 1;
        }
    free(out);
    return rc;
}

/* ---- fold ---- */
/* fold — wrap input lines to a max width (default 80).
 * Flags: -w N (width), -b (count bytes; the default counting mode here),
 * -s (break at last blank before the limit). Byte counting only, so every
 * byte advances the column by 1 and a real '\n' resets it. Mirrors GNU
 * coreutils fold_file byte-for-byte on stdout for these flags. */
static int fold_parse_width(const char *s, size_t *width, const char *prog) {
    char *end;
    unsigned long v;
    if (!isdigit((unsigned char)s[0])) {
        fprintf(stderr, "%s: invalid number of columns: '%s'\n", prog, s);
        return 1;
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (*end != '\0' || errno != 0 || v == 0) {
        fprintf(stderr, "%s: invalid number of columns: '%s'\n", prog, s);
        return 1;
    }
    *width = (size_t)v;
    return 0;
}

static int fold_stream(FILE *in, const char *name, size_t width,
                       int break_spaces, const char *prog) {
    static char *buf = NULL;
    static size_t cap = 0;
    size_t col = 0, off = 0;   /* byte mode: col == off between newlines */
    int c;

    while ((c = getc(in)) != EOF) {
        if (off + 2 > cap) {
            size_t ncap = cap ? cap * 2 : 128;
            char *nb = realloc(buf, ncap);
            if (!nb) { fprintf(stderr, "%s: memory exhausted\n", prog); exit(1); }
            buf = nb; cap = ncap;
        }
        if (c == '\n') {                 /* real newline: emit and reset column */
            buf[off++] = (char)c;
            fwrite(buf, 1, off, stdout);
            col = off = 0;
            continue;
        }
    rescan:
        col++;                           /* byte counting: one column per byte */
        if (col > width) {
            if (break_spaces) {          /* -s: break after the last blank */
                size_t le = off;
                int found = 0;
                while (le) {
                    --le;
                    if (buf[le] == ' ' || buf[le] == '\t') { found = 1; break; }
                }
                if (found) {
                    le++;                /* keep the blank on the first line */
                    fwrite(buf, 1, le, stdout);
                    putchar('\n');
                    memmove(buf, buf + le, off - le);
                    off -= le;
                    col = off;
                    goto rescan;
                }
            }
            if (off == 0) {              /* width < 1: emit char on its own */
                buf[off++] = (char)c;
                continue;
            }
            buf[off++] = '\n';
            fwrite(buf, 1, off, stdout);
            col = off = 0;
            goto rescan;
        }
        buf[off++] = (char)c;
    }

    if (off) fwrite(buf, 1, off, stdout);
    if (ferror(in)) {
        fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno));
        return 1;
    }
    return 0;
}

static int a_fold(int argc, char **argv) {
    const char *prog = argv[0];
    size_t width = 80;
    int break_spaces = 0;
    int end_opts = 0;
    int i, status = 0, nfiles = 0;
    char **files = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (!files) { fprintf(stderr, "%s: memory exhausted\n", prog); return 1; }

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!end_opts && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-') {
                char *o = a + 2;
                if (*o == '\0') { end_opts = 1; continue; }
                if (strcmp(o, "bytes") == 0) continue;   /* default mode: no-op */
                if (strcmp(o, "spaces") == 0) { break_spaces = 1; continue; }
                if (strncmp(o, "width", 5) == 0 && (o[5] == '=' || o[5] == '\0')) {
                    const char *val;
                    if (o[5] == '=') val = o + 6;
                    else {
                        if (++i >= argc) {
                            fprintf(stderr, "%s: option '--width' requires an argument\n", prog);
                            free(files); return 1;
                        }
                        val = argv[i];
                    }
                    if (fold_parse_width(val, &width, prog)) { free(files); return 1; }
                    continue;
                }
                fprintf(stderr, "%s: unrecognized option '%s'\n", prog, a);
                free(files); return 1;
            }
            char *p = a + 1;
            while (*p) {
                if (*p == 'b') { p++; }
                else if (*p == 's') { break_spaces = 1; p++; }
                else if (*p == 'w') {
                    const char *val;
                    if (p[1] != '\0') val = p + 1;
                    else {
                        if (++i >= argc) {
                            fprintf(stderr, "%s: option requires an argument -- 'w'\n", prog);
                            free(files); return 1;
                        }
                        val = argv[i];
                    }
                    if (fold_parse_width(val, &width, prog)) { free(files); return 1; }
                    break;   /* rest of this arg was the width value */
                } else {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *p);
                    free(files); return 1;
                }
            }
        } else {
            files[nfiles++] = a;
        }
    }

    if (nfiles == 0) {
        status |= fold_stream(stdin, "-", width, break_spaces, prog);
    } else {
        for (i = 0; i < nfiles; i++) {
            if (strcmp(files[i], "-") == 0) {
                status |= fold_stream(stdin, "-", width, break_spaces, prog);
            } else {
                FILE *f = fopen(files[i], "r");
                if (!f) {
                    fprintf(stderr, "%s: %s: %s\n", prog, files[i], strerror(errno));
                    status = 1;
                    continue;
                }
                status |= fold_stream(f, files[i], width, break_spaces, prog);
                fclose(f);
            }
        }
    }
    free(files);
    return status;
}

/* ---- comm ---- */
/* comm: compare two sorted files line by line (GNU-compatible, C locale).
 * Columns: 1=only in FILE1, 2=only in FILE2, 3=in both.
 * Flags -1 -2 -3 suppress a column; a present column adds one leading TAB
 * to every higher column. One operand may be "-" for stdin. */

static int comm_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    size_t m = al < bl ? al : bl;
    int r = m ? memcmp(a, b, m) : 0;
    if (r) return r;
    if (al < bl) return -1;
    if (al > bl) return 1;
    return 0;
}

static int a_comm(int argc, char **argv)
{
    int s1 = 0, s2 = 0, s3 = 0;      /* suppress column 1/2/3 */
    int endopts = 0, nfiles = 0;
    const char *files[2] = { NULL, NULL };

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopts && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "--") == 0) { endopts = 1; continue; }
            if (a[1] == '-') {
                fprintf(stderr, "comm: unrecognized option '%s'\n", a);
                fprintf(stderr, "Try 'comm --help' for more information.\n");
                return 1;
            }
            for (char *p = a + 1; *p; p++) {
                switch (*p) {
                case '1': s1 = 1; break;
                case '2': s2 = 1; break;
                case '3': s3 = 1; break;
                default:
                    fprintf(stderr, "comm: invalid option -- '%c'\n", *p);
                    fprintf(stderr, "Try 'comm --help' for more information.\n");
                    return 1;
                }
            }
            continue;
        }
        if (nfiles < 2) {
            files[nfiles++] = a;
        } else {
            fprintf(stderr, "comm: extra operand '%s'\n", a);
            fprintf(stderr, "Try 'comm --help' for more information.\n");
            return 1;
        }
    }

    if (nfiles == 0) {
        fprintf(stderr, "comm: missing operand\n");
        fprintf(stderr, "Try 'comm --help' for more information.\n");
        return 1;
    }
    if (nfiles == 1) {
        fprintf(stderr, "comm: missing operand after '%s'\n", files[0]);
        fprintf(stderr, "Try 'comm --help' for more information.\n");
        return 1;
    }

    FILE *f1, *f2;
    if (strcmp(files[0], "-") == 0) {
        f1 = stdin;
    } else {
        f1 = fopen(files[0], "r");
        if (!f1) {
            fprintf(stderr, "comm: %s: %s\n", files[0], strerror(errno));
            return 1;
        }
    }
    if (strcmp(files[1], "-") == 0) {
        f2 = stdin;
    } else {
        f2 = fopen(files[1], "r");
        if (!f2) {
            fprintf(stderr, "comm: %s: %s\n", files[1], strerror(errno));
            if (f1 != stdin) fclose(f1);
            return 1;
        }
    }

    char *l1 = NULL, *l2 = NULL;
    size_t c1 = 0, c2 = 0;
    ssize_t n1, n2;
    int have1, have2;

    n1 = getline(&l1, &c1, f1); have1 = (n1 >= 0);
    n2 = getline(&l2, &c2, f2); have2 = (n2 >= 0);

    while (have1 || have2) {
        int order;
        if (!have1)      order = 1;              /* file1 exhausted -> col2 */
        else if (!have2) order = -1;             /* file2 exhausted -> col1 */
        else             order = comm_cmp(l1, (size_t)n1, l2, (size_t)n2);

        if (order == 0) {                        /* column 3: in both */
            if (!s3) {
                if (!s1) putchar('\t');
                if (!s2) putchar('\t');
                fwrite(l2, 1, (size_t)n2, stdout);
            }
        } else if (order < 0) {                  /* column 1: only in FILE1 */
            if (!s1)
                fwrite(l1, 1, (size_t)n1, stdout);
        } else {                                 /* column 2: only in FILE2 */
            if (!s2) {
                if (!s1) putchar('\t');
                fwrite(l2, 1, (size_t)n2, stdout);
            }
        }

        if (order <= 0) { n1 = getline(&l1, &c1, f1); have1 = (n1 >= 0); }
        if (order >= 0) { n2 = getline(&l2, &c2, f2); have2 = (n2 >= 0); }
    }

    free(l1);
    free(l2);

    int rc = 0;
    if (ferror(f1)) {
        fprintf(stderr, "comm: %s: %s\n", files[0], strerror(errno));
        rc = 1;
    }
    if (ferror(f2)) {
        fprintf(stderr, "comm: %s: %s\n", files[1], strerror(errno));
        rc = 1;
    }
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "comm: write error: %s\n", strerror(errno));
        rc = 1;
    }

    if (f1 != stdin) fclose(f1);
    if (f2 != stdin) fclose(f2);
    return rc;
}

/* ---- paste ---- */
static void paste_out_delim(const char *delims, int off, int len) {
    if (len > 0) fwrite(delims + off, 1, (size_t)len, stdout);
}

/* Parse -d LIST into single-byte delimiters, honoring GNU escapes
   (\t \n \r \f \b \v \\ and \0 => empty delimiter).  Returns 0 on success,
   1 if the list ends with an unescaped backslash, -1 on allocation error. */
static int paste_collapse(const char *name, const char *list,
                          char **out_bytes, int **out_lens, int *out_n) {
    size_t slen = strlen(list);
    char *bytes = malloc(slen + 1);
    int *lens = malloc((slen ? slen : 1) * sizeof(int));
    if (!bytes || !lens) {
        free(bytes); free(lens);
        fprintf(stderr, "%s: memory exhausted\n", name);
        return -1;
    }
    char *w = bytes;
    int idx = 0;
    const char *s = list;
    int backslash_at_end = 0;
    while (*s) {
        if (*s == '\\') {
            s++;
            if (*s == '\0') { backslash_at_end = 1; break; }
            if (*s == '0') { s++; lens[idx++] = 0; continue; }
            {
                char c;
                switch (*s) {
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'v': c = '\v'; break;
                    case '\\': c = '\\'; break;
                    default: goto copy_char;
                }
                *w++ = c;
                s++;
                lens[idx++] = 1;
                continue;
            }
        }
    copy_char:
        *w++ = *s++;
        lens[idx++] = 1;
    }
    *w = '\0';
    if (idx == 0) { lens[0] = 0; idx = 1; }
    *out_bytes = bytes;
    *out_lens = lens;
    *out_n = idx;
    if (backslash_at_end) {
        fprintf(stderr,
                "%s: delimiter list ends with an unescaped backslash: %s\n",
                name, list);
        return 1;
    }
    return 0;
}

static int paste_parallel(const char *name, int nfiles, char **fnames,
                          const char *delims, const int *dlens, int ndelims,
                          int line_delim) {
    int ok = 1;
    FILE **fp = malloc(sizeof(FILE *) * (size_t)nfiles);
    char *delbuf = malloc((size_t)nfiles + 1);
    if (!fp || !delbuf) {
        free(fp); free(delbuf);
        fprintf(stderr, "%s: memory exhausted\n", name);
        return 1;
    }
    /* Open every file up front (GNU parallel semantics): a failed open is
       fatal and produces no output. */
    for (int f = 0; f < nfiles; f++) {
        if (strcmp(fnames[f], "-") == 0) {
            fp[f] = stdin;
        } else {
            fp[f] = fopen(fnames[f], "r");
            if (!fp[f]) {
                fprintf(stderr, "%s: %s: %s\n", name, fnames[f], strerror(errno));
                for (int g = 0; g < f; g++)
                    if (fp[g] && fp[g] != stdin) fclose(fp[g]);
                free(fp); free(delbuf);
                return 1;
            }
        }
    }

    int files_open = nfiles;
    while (files_open) {
        int somedone = 0;
        int delimidx = 0, delimoff = 0, delims_saved = 0;
        for (int i = 0; i < nfiles && files_open; i++) {
            int chr = EOF, err = 0, sometodo = 0;
            if (fp[i]) {
                chr = getc(fp[i]);
                err = errno;
                if (chr != EOF && delims_saved) {
                    fwrite(delbuf, 1, (size_t)delims_saved, stdout);
                    delims_saved = 0;
                }
                while (chr != EOF) {
                    sometodo = 1;
                    if (chr == line_delim) break;
                    putchar(chr);
                    chr = getc(fp[i]);
                    err = errno;
                }
            }

            if (!sometodo) {
                if (fp[i]) {
                    if (ferror(fp[i])) {
                        fprintf(stderr, "%s: %s: %s\n", name, fnames[i],
                                strerror(err));
                        ok = 0;
                    }
                    if (fp[i] == stdin) clearerr(fp[i]);
                    else fclose(fp[i]);
                    fp[i] = NULL;
                    files_open--;
                }
                if (i + 1 == nfiles) {
                    if (somedone) {
                        if (delims_saved) {
                            fwrite(delbuf, 1, (size_t)delims_saved, stdout);
                            delims_saved = 0;
                        }
                        putchar(line_delim);
                    }
                    continue;
                } else {
                    int len = dlens[delimidx];
                    if (len > 0) {
                        memcpy(delbuf + delims_saved, delims + delimoff,
                               (size_t)len);
                        delims_saved += len;
                    }
                    delimoff += len;
                    if (++delimidx == ndelims) { delimidx = 0; delimoff = 0; }
                }
            } else {
                somedone = 1;
                if (i + 1 != nfiles) {
                    paste_out_delim(delims, delimoff, dlens[delimidx]);
                    delimoff += dlens[delimidx];
                    if (++delimidx == ndelims) { delimidx = 0; delimoff = 0; }
                } else {
                    int c = (chr == EOF) ? line_delim : chr;
                    putchar(c);
                }
            }
        }
    }
    free(fp);
    free(delbuf);
    return ok ? 0 : 1;
}

static int paste_serial(const char *name, int nfiles, char **fnames,
                        const char *delims, const int *dlens, int ndelims,
                        int line_delim) {
    int ok = 1;
    for (int f = 0; f < nfiles; f++) {
        FILE *fp;
        int is_stdin = (strcmp(fnames[f], "-") == 0);
        if (is_stdin) {
            fp = stdin;
        } else {
            fp = fopen(fnames[f], "r");
            if (!fp) {
                fprintf(stderr, "%s: %s: %s\n", name, fnames[f], strerror(errno));
                ok = 0;
                continue;
            }
        }
        int delimidx = 0, delimoff = 0;
        int charold, charnew;
        charold = getc(fp);
        int se = errno;
        if (charold != EOF) {
            while ((charnew = getc(fp)) != EOF) {
                if (charold == line_delim) {
                    paste_out_delim(delims, delimoff, dlens[delimidx]);
                    delimoff += dlens[delimidx];
                    if (++delimidx == ndelims) { delimidx = 0; delimoff = 0; }
                } else {
                    putchar(charold);
                }
                charold = charnew;
            }
            se = errno;
            putchar(charold);
        }
        if (charold != line_delim) putchar(line_delim);
        if (!ferror(fp)) se = 0;
        if (is_stdin) clearerr(fp);
        else if (fclose(fp) != 0 && !se) se = errno;
        if (se) {
            fprintf(stderr, "%s: %s: %s\n", name, fnames[f], strerror(se));
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}

static int a_paste(int argc, char **argv) {
    const char *name = argv[0];
    int serial = 0;
    const char *dlist = "\t";     /* default: single TAB */
    int line_delim = '\n';
    char **files = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (!files) { fprintf(stderr, "%s: memory exhausted\n", name); return 1; }
    int nfiles = 0;
    int no_more_opts = 0;

    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!no_more_opts && arg[0] == '-' && arg[1] != '\0') {
            if (arg[1] == '-' && arg[2] == '\0') { no_more_opts = 1; continue; }
            if (arg[1] == '-') {
                if (strcmp(arg, "--serial") == 0) { serial = 1; continue; }
                if (strncmp(arg, "--delimiters=", 13) == 0) {
                    dlist = arg + 13; continue;
                }
                if (strcmp(arg, "--delimiters") == 0) {
                    if (++i < argc) { dlist = argv[i]; continue; }
                    fprintf(stderr, "%s: option '--delimiters' requires an argument\n", name);
                    free(files); return 1;
                }
                fprintf(stderr, "%s: unrecognized option '%s'\n", name, arg);
                free(files); return 1;
            }
            const char *p = arg + 1;
            while (*p) {
                if (*p == 's') { serial = 1; p++; }
                else if (*p == 'd') {
                    if (*(p + 1)) { dlist = p + 1; }
                    else if (++i < argc) { dlist = argv[i]; }
                    else {
                        fprintf(stderr, "%s: option requires an argument -- 'd'\n", name);
                        free(files); return 1;
                    }
                    break;
                } else {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", name, *p);
                    free(files); return 1;
                }
            }
            continue;
        }
        files[nfiles++] = arg;
    }

    if (nfiles == 0) files[nfiles++] = "-";

    char *dbytes = NULL;
    int *dlens = NULL, ndelims = 0;
    int rc = paste_collapse(name, dlist, &dbytes, &dlens, &ndelims);
    if (rc != 0) { free(files); free(dbytes); free(dlens); return 1; }

    int status;
    if (serial)
        status = paste_serial(name, nfiles, files, dbytes, dlens, ndelims, line_delim);
    else
        status = paste_parallel(name, nfiles, files, dbytes, dlens, ndelims, line_delim);

    free(files);
    free(dbytes);
    free(dlens);
    return status;
}


/* ------------------------------------------------- extended applets 3 --- */
/* ---- grep ---- */
/* ================= grep ================= */

typedef struct { int fixed; regex_t re; const char *lit; size_t litlen; } grep_pat;

static int grep_wordch(int c) { return isalnum((unsigned char)c) || c == '_'; }

/* find leftmost occurrence of literal in line[pos..len) */
static int grep_fixfind(const char *line, size_t len, size_t pos,
                        const char *lit, size_t ll, int icase,
                        size_t *so, size_t *eo)
{
    if (ll == 0) { if (pos > len) return 0; *so = pos; *eo = pos; return 1; }
    if (pos > len || ll > len) return 0;
    for (size_t i = pos; i + ll <= len; i++) {
        size_t j = 0;
        for (; j < ll; j++) {
            int a = (unsigned char)line[i + j], b = (unsigned char)lit[j];
            if (icase) { a = tolower(a); b = tolower(b); }
            if (a != b) break;
        }
        if (j == ll) { *so = i; *eo = i + ll; return 1; }
    }
    return 0;
}

/* leftmost match across all patterns at position >= pos (no -w/-x filtering) */
static int grep_leftmost(grep_pat *pats, int np, const char *line, size_t len,
                         size_t pos, int icase, size_t *rso, size_t *reo)
{
    int found = 0; size_t bso = 0, beo = 0;
    for (int k = 0; k < np; k++) {
        size_t so, eo; int f = 0;
        if (pats[k].fixed) {
            f = grep_fixfind(line, len, pos, pats[k].lit, pats[k].litlen, icase, &so, &eo);
        } else {
            regmatch_t m;
            int rc = regexec(&pats[k].re, line + pos, 1, &m, pos > 0 ? REG_NOTBOL : 0);
            if (rc == 0) { so = pos + (size_t)m.rm_so; eo = pos + (size_t)m.rm_eo; f = 1; }
        }
        if (f && (!found || so < bso || (so == bso && eo > beo))) {
            found = 1; bso = so; beo = eo;
        }
    }
    if (found) { *rso = bso; *reo = beo; }
    return found;
}

/* scan for first match at >= start satisfying -w (if wflag) */
static int grep_scan(grep_pat *pats, int np, const char *line, size_t len,
                     size_t start, int icase, int wflag, size_t *mso, size_t *meo)
{
    size_t pos = start;
    while (pos <= len) {
        size_t so, eo;
        if (!grep_leftmost(pats, np, line, len, pos, icase, &so, &eo)) return 0;
        if (!wflag) { *mso = so; *meo = eo; return 1; }
        int okl = (so == 0) || !grep_wordch(line[so - 1]);
        int okr = (eo == len) || !grep_wordch(line[eo]);
        if (okl && okr) { *mso = so; *meo = eo; return 1; }
        pos = so + 1;
    }
    return 0;
}

/* does any pattern match the entire line [0,len) ? (for -x) */
static int grep_wholeline(grep_pat *pats, int np, const char *line, size_t len, int icase)
{
    for (int k = 0; k < np; k++) {
        if (pats[k].fixed) {
            if (pats[k].litlen == len) {
                size_t j; int eq = 1;
                for (j = 0; j < len; j++) {
                    int a = (unsigned char)line[j], b = (unsigned char)pats[k].lit[j];
                    if (icase) { a = tolower(a); b = tolower(b); }
                    if (a != b) { eq = 0; break; }
                }
                if (eq) return 1;
            }
        } else {
            regmatch_t m;
            if (regexec(&pats[k].re, line, 1, &m, 0) == 0 &&
                m.rm_so == 0 && (size_t)m.rm_eo == len)
                return 1;
        }
    }
    return 0;
}

static int a_grep(int argc, char **argv)
{
    int iflag = 0, vflag = 0, cflag = 0, nflag = 0, lflag = 0, oflag = 0;
    int wflag = 0, xflag = 0, qflag = 0, hflag = 0, Hflag = 0, Eflag = 0, Fflag = 0;
    const char **raw = NULL; int nraw = 0, rawcap = 0;
    const char **files = NULL; int nfile = 0, filecap = 0;
    int nomore = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!nomore && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-' && a[2] == '\0') { nomore = 1; continue; }
            if (a[1] == '-') { /* long option */
                char *opt = a + 2; const char *val = NULL;
                char *eq = strchr(opt, '=');
                size_t olen = eq ? (size_t)(eq - opt) : strlen(opt);
                if (eq) val = eq + 1;
                #define LMATCH(s) (olen == strlen(s) && strncmp(opt, s, olen) == 0)
                if (LMATCH("ignore-case")) iflag = 1;
                else if (LMATCH("invert-match")) vflag = 1;
                else if (LMATCH("count")) cflag = 1;
                else if (LMATCH("line-number")) nflag = 1;
                else if (LMATCH("files-with-matches")) lflag = 1;
                else if (LMATCH("only-matching")) oflag = 1;
                else if (LMATCH("word-regexp")) wflag = 1;
                else if (LMATCH("line-regexp")) xflag = 1;
                else if (LMATCH("quiet") || LMATCH("silent")) qflag = 1;
                else if (LMATCH("no-filename")) hflag = 1;
                else if (LMATCH("with-filename")) Hflag = 1;
                else if (LMATCH("extended-regexp")) Eflag = 1;
                else if (LMATCH("fixed-strings")) Fflag = 1;
                else if (LMATCH("regexp")) {
                    if (!val) { if (i + 1 >= argc) goto usage_err; val = argv[++i]; }
                    if (nraw >= rawcap) { rawcap = rawcap ? rawcap * 2 : 8; raw = realloc(raw, rawcap * sizeof(*raw)); }
                    raw[nraw++] = val;
                } else {
                    fprintf(stderr, "grep: unrecognized option '%s'\n", a);
                    goto usage_err;
                }
                #undef LMATCH
                continue;
            }
            int stop = 0;
            for (char *p = a + 1; *p && !stop; p++) {
                switch (*p) {
                case 'i': iflag = 1; break;
                case 'v': vflag = 1; break;
                case 'c': cflag = 1; break;
                case 'n': nflag = 1; break;
                case 'l': lflag = 1; break;
                case 'o': oflag = 1; break;
                case 'w': wflag = 1; break;
                case 'x': xflag = 1; break;
                case 'q': qflag = 1; break;
                case 'h': hflag = 1; break;
                case 'H': Hflag = 1; break;
                case 'E': Eflag = 1; break;
                case 'F': Fflag = 1; break;
                case 'e': {
                    const char *val;
                    if (p[1]) val = p + 1;
                    else { if (i + 1 >= argc) goto usage_err; val = argv[++i]; }
                    if (nraw >= rawcap) { rawcap = rawcap ? rawcap * 2 : 8; raw = realloc(raw, rawcap * sizeof(*raw)); }
                    raw[nraw++] = val;
                    stop = 1; break;
                }
                default:
                    fprintf(stderr, "grep: invalid option -- '%c'\n", *p);
                    goto usage_err;
                }
            }
        } else {
            if (nfile >= filecap) { filecap = filecap ? filecap * 2 : 8; files = realloc(files, filecap * sizeof(*files)); }
            files[nfile++] = a;
        }
    }

    if (nraw == 0) {
        if (nfile == 0) goto usage_err;
        if (nraw >= rawcap) { rawcap = rawcap ? rawcap * 2 : 8; raw = realloc(raw, rawcap * sizeof(*raw)); }
        raw[nraw++] = files[0];
        for (int k = 1; k < nfile; k++) files[k - 1] = files[k];
        nfile--;
    }

    /* compile patterns */
    grep_pat *pats = calloc(nraw, sizeof(*pats));
    int cflags = (Eflag ? REG_EXTENDED : 0) | (iflag ? REG_ICASE : 0);
    for (int k = 0; k < nraw; k++) {
        if (Fflag || raw[k][0] == '\0') {
            pats[k].fixed = 1;
            pats[k].lit = raw[k];
            pats[k].litlen = strlen(raw[k]);
        } else {
            int e = regcomp(&pats[k].re, raw[k], cflags);
            if (e) {
                char ebuf[256];
                regerror(e, &pats[k].re, ebuf, sizeof(ebuf));
                fprintf(stderr, "grep: %s\n", ebuf);
                for (int j = 0; j < k; j++) if (!pats[j].fixed) regfree(&pats[j].re);
                free(pats); free(raw); free(files);
                return 2;
            }
        }
    }

    int show_fn = hflag ? 0 : (Hflag ? 1 : ((nfile > 1) ? 1 : 0));

    int any_match = 0, had_error = 0;
    int ninputs = nfile > 0 ? nfile : 1;
    char *buf = NULL; size_t bufcap = 0;
    int ret = 1;

    for (int fi = 0; fi < ninputs; fi++) {
        const char *name;
        const char *disp;
        FILE *in;
        if (nfile == 0) { name = "-"; disp = "(standard input)"; in = stdin; }
        else {
            name = files[fi];
            if (strcmp(name, "-") == 0) { disp = "(standard input)"; in = stdin; }
            else {
                struct stat st;
                if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
                    fflush(stdout);
                    fprintf(stderr, "grep: %s: Is a directory\n", name);
                    had_error = 1; continue;
                }
                in = fopen(name, "r");
                if (!in) {
                    fflush(stdout);
                    fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
                    had_error = 1; continue;
                }
                disp = name;
            }
        }

        long lineno = 0;
        long count = 0;
        ssize_t nread;
        while ((nread = getline(&buf, &bufcap, in)) != -1) {
            lineno++;
            size_t clen = (size_t)nread;
            int had_nl = (nread > 0 && buf[nread - 1] == '\n');
            if (had_nl) clen = (size_t)nread - 1;
            char saved = buf[clen];
            buf[clen] = '\0';

            int raw_match;
            if (xflag) raw_match = grep_wholeline(pats, nraw, buf, clen, iflag);
            else { size_t s, e; raw_match = grep_scan(pats, nraw, buf, clen, 0, iflag, wflag, &s, &e); }
            int sel = vflag ? !raw_match : raw_match;

            if (sel) {
                any_match = 1;
                if (qflag) {
                    if (in != stdin) fclose(in);
                    ret = 0; goto done;
                }
                if (lflag) {
                    fputs(disp, stdout); fputc('\n', stdout);
                    break; /* stop this file */
                }
                if (cflag) { count++; }
                else if (oflag) {
                    if (!vflag) {
                        if (xflag) {
                            if (show_fn) { fputs(disp, stdout); fputc(':', stdout); }
                            if (nflag) printf("%ld:", lineno);
                            fwrite(buf, 1, clen, stdout); fputc('\n', stdout);
                        } else {
                            size_t pos = 0, s, e;
                            while (grep_scan(pats, nraw, buf, clen, pos, iflag, wflag, &s, &e)) {
                                if (e > s) {
                                    if (show_fn) { fputs(disp, stdout); fputc(':', stdout); }
                                    if (nflag) printf("%ld:", lineno);
                                    fwrite(buf + s, 1, e - s, stdout); fputc('\n', stdout);
                                    pos = e;
                                } else pos = s + 1;
                            }
                        }
                    }
                } else {
                    if (show_fn) { fputs(disp, stdout); fputc(':', stdout); }
                    if (nflag) printf("%ld:", lineno);
                    fwrite(buf, 1, clen, stdout);
                    fputc('\n', stdout);
                }
            }
            buf[clen] = saved;
        }

        if (!lflag && cflag && !qflag) {
            if (show_fn) { fputs(disp, stdout); fputc(':', stdout); }
            printf("%ld\n", count);
        }

        if (in != stdin && ferror(in)) {
            fflush(stdout);
            fprintf(stderr, "grep: %s: %s\n", disp, strerror(errno));
            had_error = 1;
        }
        if (in != stdin) fclose(in);
    }

    ret = had_error ? 2 : (any_match ? 0 : 1);

done:
    free(buf);
    for (int k = 0; k < nraw; k++) if (!pats[k].fixed) regfree(&pats[k].re);
    free(pats); free(raw); free(files);
    return ret;

usage_err:
    fprintf(stderr, "Usage: grep [OPTION]... PATTERNS [FILE]...\n");
    free(raw); free(files);
    return 2;
}

/* ---- chmod ---- */
/* ---- chmod ---- */
struct chmod_change {
    char  op;         /* '+', '-', '='                      */
    int   flag;       /* 0=ordinary, 1=X-if-any-x, 2=copy    */
    mode_t affected;  /* who-mask (S_ISUID|S_IRWXU, ...)      */
    mode_t value;     /* raw perm bits from the permlist     */
    mode_t mentioned; /* special bits explicitly named       */
    mode_t copysrc;   /* copy op: source class mask          */
};

/* Parse MODE into CH[] (capacity CAP). Returns #changes, or -1 on error. */
static int chmod_compile(const char *s, struct chmod_change *ch, int cap) {
    int n = 0;

    if (*s >= '0' && *s <= '7') {                 /* octal (absolute) */
        unsigned long v = 0;
        const char *p = s;
        while (*p >= '0' && *p <= '7') { v = v * 8 + (unsigned)(*p - '0'); p++; }
        if (*p != '\0' || (v & ~07777UL)) return -1;
        if (cap < 1) return -1;
        ch[0].op = '='; ch[0].flag = 0; ch[0].affected = 07777;
        ch[0].value = (mode_t)v; ch[0].mentioned = 07777; ch[0].copysrc = 0;
        return 1;
    }

    while (*s) {
        mode_t affected = 0;
        int sawwho = 0;
        for (;; s++) {                            /* who */
            if      (*s == 'u') affected |= S_ISUID | S_IRWXU;
            else if (*s == 'g') affected |= S_ISGID | S_IRWXG;
            else if (*s == 'o') affected |= S_ISVTX | S_IRWXO;
            else if (*s == 'a') affected |= 07777;
            else break;
            sawwho = 1;
        }
        if (!sawwho) affected = 07777;            /* empty who == all */

        if (*s != '+' && *s != '-' && *s != '=') return -1;

        while (*s == '+' || *s == '-' || *s == '=') {   /* one or more actions */
            char op = *s++;
            int  flag = 0;
            mode_t value = 0, copysrc = 0, mentioned = 0;

            if (*s == 'u' || *s == 'g' || *s == 'o') {  /* perm-copy */
                flag = 2;
                copysrc = (*s == 'u') ? S_IRWXU : (*s == 'g') ? S_IRWXG : S_IRWXO;
                s++;
            } else {                                    /* permlist */
                for (;; s++) {
                    if      (*s == 'r') value |= 0444;
                    else if (*s == 'w') value |= 0222;
                    else if (*s == 'x') value |= 0111;
                    else if (*s == 'X') flag = 1;
                    else if (*s == 's') value |= S_ISUID | S_ISGID;
                    else if (*s == 't') value |= S_ISVTX;
                    else break;
                }
                mentioned = (value & affected) & (S_ISUID | S_ISGID | S_ISVTX);
            }

            if (n >= cap) return -1;
            ch[n].op = op; ch[n].flag = flag; ch[n].affected = affected;
            ch[n].value = value; ch[n].mentioned = mentioned; ch[n].copysrc = copysrc;
            n++;
        }

        if (*s == ',') { s++; continue; }
        if (*s == '\0') break;
        return -1;
    }
    return n > 0 ? n : -1;
}

/* Apply the compiled changes to OLDMODE (only the low 12 bits matter). */
static mode_t chmod_adjust(mode_t oldmode, int isdir,
                           const struct chmod_change *ch, int n) {
    mode_t newmode = oldmode & 07777;
    int i;
    for (i = 0; i < n; i++) {
        mode_t affected = ch[i].affected;
        mode_t value = ch[i].value;
        mode_t mentioned = ch[i].mentioned;
        mode_t omit, preserved;

        if (ch[i].flag == 2) {                    /* copy existing */
            mode_t v = ch[i].copysrc & newmode;
            value = (v & 0444 ? 0444 : 0) | (v & 0222 ? 0222 : 0) | (v & 0111 ? 0111 : 0);
        } else if (ch[i].flag == 1) {             /* X: exec if dir or any x set */
            if (isdir || (newmode & 0111)) value |= 0111;
        }

        value &= affected;
        omit = (isdir ? 0 : (mode_t)(S_ISUID | S_ISGID)) & ~mentioned;
        value &= ~omit;

        switch (ch[i].op) {
        case '=':
            preserved = ~affected | ((isdir ? (mode_t)(S_ISUID | S_ISGID) : 0) & ~mentioned);
            newmode = (newmode & preserved) | value;
            break;
        case '+':
            newmode |= value;
            break;
        case '-':
            newmode &= ~value;
            break;
        }
    }
    return newmode & 07777;
}

/* Change one path (dereferencing top-level symlinks); recurse when asked. */
static int chmod_apply(const char *path, int toplevel, int recursive,
                       const struct chmod_change *ch, int n) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "chmod: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISLNK(st.st_mode)) {
        if (!toplevel) return 0;                  /* never follow symlinks while recursing */
        if (stat(path, &st) != 0) {
            fprintf(stderr, "chmod: cannot access '%s': %s\n", path, strerror(errno));
            return 1;
        }
    }

    int rc = 0;
    int isdir = S_ISDIR(st.st_mode);
    mode_t oldbits = st.st_mode & 07777;
    mode_t newbits = chmod_adjust(st.st_mode, isdir, ch, n);

    /* Read child names before touching the directory's own bits. */
    char **names = NULL;
    int nnames = 0, cap = 0;
    if (recursive && isdir) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (nnames >= cap) {
                    int ncap = cap ? cap * 2 : 16;
                    char **t = realloc(names, (size_t)ncap * sizeof *t);
                    if (!t) { fprintf(stderr, "chmod: memory exhausted\n"); rc = 1; break; }
                    names = t; cap = ncap;
                }
                names[nnames] = strdup(e->d_name);
                if (!names[nnames]) { fprintf(stderr, "chmod: memory exhausted\n"); rc = 1; break; }
                nnames++;
            }
            closedir(d);
        } else {
            fprintf(stderr, "chmod: cannot read directory '%s': %s\n", path, strerror(errno));
            rc = 1;
        }
    }

    if (newbits != oldbits) {
        if (chmod(path, newbits) != 0) {
            fprintf(stderr, "chmod: changing permissions of '%s': %s\n", path, strerror(errno));
            rc = 1;
        }
    }

    if (nnames > 0) {
        size_t plen = strlen(path);
        int slash = (plen > 0 && path[plen - 1] == '/');
        int j;
        for (j = 0; j < nnames; j++) {
            size_t need = plen + 1 + strlen(names[j]) + 1;
            char *child = malloc(need);
            if (child) {
                if (slash) snprintf(child, need, "%s%s", path, names[j]);
                else       snprintf(child, need, "%s/%s", path, names[j]);
                if (chmod_apply(child, 0, recursive, ch, n) != 0) rc = 1;
                free(child);
            } else {
                fprintf(stderr, "chmod: memory exhausted\n"); rc = 1;
            }
            free(names[j]);
        }
    }
    free(names);
    return rc;
}

static int a_chmod(int argc, char **argv) {
    int recursive = 0;
    int i = 1;
    char *modestr = NULL;

    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') { modestr = a; i++; break; }   /* operand -> MODE */
        if (a[1] == '-') {
            if (a[2] == '\0') {                        /* "--" */
                i++;
                if (i < argc) { modestr = argv[i]; i++; }
                break;
            }
            if (!strcmp(a, "--recursive")) { recursive = 1; continue; }
            if (!strcmp(a, "--changes") || !strcmp(a, "--verbose") ||
                !strcmp(a, "--quiet")   || !strcmp(a, "--silent")  ||
                !strcmp(a, "--no-preserve-root") || !strcmp(a, "--preserve-root")) continue;
            fprintf(stderr, "chmod: unrecognized option '%s'\n", a);
            fprintf(stderr, "Try 'chmod --help' for more information.\n");
            return 1;
        }
        {   /* single dash: option bundle only if every char is an option letter */
            int isopt = 1;
            char *c;
            for (c = a + 1; *c; c++)
                if (!strchr("RcvfHLP", *c)) { isopt = 0; break; }
            if (!isopt) { modestr = a; i++; break; }   /* looks like a leading-dash MODE */
            for (c = a + 1; *c; c++)
                if (*c == 'R') recursive = 1;          /* c,v,f,H,L,P accepted, no-op */
        }
    }

    if (!modestr) {
        fprintf(stderr, "chmod: missing operand\n");
        fprintf(stderr, "Try 'chmod --help' for more information.\n");
        return 1;
    }
    if (i >= argc) {
        fprintf(stderr, "chmod: missing operand after '%s'\n", modestr);
        fprintf(stderr, "Try 'chmod --help' for more information.\n");
        return 1;
    }

    size_t cap = strlen(modestr) + 2;
    struct chmod_change *ch = malloc(cap * sizeof *ch);
    if (!ch) { fprintf(stderr, "chmod: memory exhausted\n"); return 1; }
    int n = chmod_compile(modestr, ch, (int)cap);
    if (n < 0) {
        fprintf(stderr, "chmod: invalid mode: '%s'\n", modestr);
        fprintf(stderr, "Try 'chmod --help' for more information.\n");
        free(ch);
        return 1;
    }

    int rc = 0;
    for (; i < argc; i++)
        if (chmod_apply(argv[i], 1, recursive, ch, n) != 0) rc = 1;

    free(ch);
    return rc;
}

/* ---- du ---- */
/* du — estimate file space usage in 1024-byte blocks.
 *
 * Sizes are accumulated as raw st_blocks (512-byte units). GNU du keeps the
 * disk-usage total in bytes and converts once, at print time, to the output
 * block size (1024) using human_readable() whose default rounding mode is
 * human_ceiling (opts == 0). So blocks_1024 = ceil(total_512 / 2) = (n+1)/2,
 * applied to the SUM (not per file). For -h we reuse ls_humansize(), which is
 * the same 1024-based, ceiling-rounded formatter GNU's human_readable drives.
 * Traversal is physical (lstat, no symlink deref) and post-order: a directory's
 * own line is emitted after all of its children, in raw readdir order (this is
 * what fts with a NULL comparator yields). Files with st_nlink>1 are counted
 * only once across the whole run, matching GNU's hard-link de-duplication. */

static int du_a, du_s, du_h, du_status;
static struct du_hl { dev_t dev; ino_t ino; } *du_hls;
static size_t du_hlcnt, du_hlcap;

/* Return 1 if (dev,ino) was already recorded; otherwise record it and return 0.
 * Only called for non-directory files with a link count above one. */
static int du_dup(dev_t dev, ino_t ino) {
    for (size_t i = 0; i < du_hlcnt; i++)
        if (du_hls[i].dev == dev && du_hls[i].ino == ino) return 1;
    if (du_hlcnt == du_hlcap) {
        du_hlcap = du_hlcap ? du_hlcap * 2 : 64;
        du_hls = realloc(du_hls, du_hlcap * sizeof *du_hls);
    }
    du_hls[du_hlcnt].dev = dev;
    du_hls[du_hlcnt].ino = ino;
    du_hlcnt++;
    return 0;
}

/* Join dir and name with a single '/', unless dir already ends in one. */
static char *du_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = (dl > 0 && dir[dl - 1] == '/') ? 0 : 1;
    char *p = malloc(dl + slash + nl + 1);
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    memcpy(p + dl + slash, name, nl);
    p[dl + slash + nl] = 0;
    return p;
}

/* Emit one "<size>\t<path>" line for a subtotal given in 512-byte blocks. */
static void du_emit(long long blk512, const char *path) {
    if (du_h) {
        char b[32];
        ls_humansize(blk512 * 512, b);
        printf("%s\t%s\n", b, path);
    } else {
        printf("%lld\t%s\n", (blk512 + 1) / 2, path);   /* ceil to 1024-blocks */
    }
}

/* Recurse into path, return its total disk usage in 512-byte blocks, and print
 * lines as the flags dictate. depth 0 is a command-line operand. */
static long long du_walk(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
        du_status = 1;
        return 0;
    }
    long long total = (long long) st.st_blocks;   /* this node's own blocks */

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "du: cannot read directory '%s': %s\n", path, strerror(errno));
            du_status = 1;
        } else {
            struct dirent *de;
            while ((de = readdir(d))) {
                const char *nm = de->d_name;
                if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                    continue;
                char *child = du_join(path, nm);
                total += du_walk(child, depth + 1);
                free(child);
            }
            closedir(d);
        }
        /* Directories always get a line unless -s suppresses interior lines. */
        if (!du_s || depth == 0) du_emit(total, path);
    } else {
        /* Skip hard-linked files already counted elsewhere. */
        if (st.st_nlink > 1 && du_dup(st.st_dev, st.st_ino)) return 0;
        /* Files: always printed as an operand; otherwise only with -a. */
        if (depth == 0 || du_a) du_emit(total, path);
    }
    return total;
}

static int a_du(int argc, char **argv) {
    du_a = du_s = du_h = du_status = 0;
    du_hls = NULL; du_hlcnt = du_hlcap = 0;
    int c_flag = 0, endopt = 0;

    char **ops = malloc(sizeof(char *) * (argc + 1));
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopt && a[0] == '-' && a[1]) {
            if (a[1] == '-' && a[2] == 0) { endopt = 1; continue; }
            for (int j = 1; a[j]; j++) {
                switch (a[j]) {
                    case 's': du_s = 1; break;
                    case 'h': du_h = 1; break;
                    case 'a': du_a = 1; break;
                    case 'c': c_flag = 1; break;
                    default:
                        fprintf(stderr, "du: invalid option -- '%c'\n"
                                        "Try 'du --help' for more information.\n", a[j]);
                        free(ops); free(du_hls);
                        return 1;
                }
            }
        } else {
            ops[nops++] = a;
        }
    }

    if (du_a && du_s) {
        fprintf(stderr, "du: cannot both summarize and show all entries\n");
        free(ops); free(du_hls);
        return 1;
    }

    if (nops == 0) ops[nops++] = ".";

    long long grand = 0;
    for (int i = 0; i < nops; i++) {
        char *op = strdup(ops[i]);            /* strip trailing slashes, keep "/" */
        size_t L = strlen(op);
        while (L > 1 && op[L - 1] == '/') op[--L] = 0;
        grand += du_walk(op, 0);
        free(op);
    }

    if (c_flag) du_emit(grand, "total");

    free(ops); free(du_hls);
    return du_status;
}

/* ---- printf ---- */
/* ---- printf applet ---- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"

static int printf_rc = 0;

/* Interpret one backslash escape. *pp points just past the backslash and is
 * advanced past the consumed characters. octal0 selects echo-style octal
 * (\0NNN, a leading 0 is optional) used by %b; the format string uses \NNN. */
static void printf_esc(const char **pp, int octal0) {
    const char *p = *pp;
    int c = (unsigned char)*p;

    if (c == 'x' && isxdigit((unsigned char)p[1])) {
        int v = 0, n = 0;
        p++;
        while (n < 2 && isxdigit((unsigned char)*p)) {
            int d = (unsigned char)*p;
            d = (d >= '0' && d <= '9') ? d - '0'
                                       : (tolower(d) - 'a' + 10);
            v = v * 16 + d;
            p++; n++;
        }
        putchar(v);
        *pp = p; return;
    }
    if (c >= '0' && c <= '7') {
        int v = 0, n = 0;
        if (octal0 && *p == '0') p++;
        while (n < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p - '0');
            p++; n++;
        }
        putchar(v);
        *pp = p; return;
    }
    switch (c) {
        case 'a':  putchar('\a');   p++; break;
        case 'b':  putchar('\b');   p++; break;
        case 'c':  fflush(stdout); exit(0);
        case 'e':  putchar(0x1B);   p++; break;
        case 'f':  putchar('\f');   p++; break;
        case 'n':  putchar('\n');   p++; break;
        case 'r':  putchar('\r');   p++; break;
        case 't':  putchar('\t');   p++; break;
        case 'v':  putchar('\v');   p++; break;
        case '\\': putchar('\\');   p++; break;
        case '"':  putchar('"');    p++; break;
        case '\'': putchar('\'');   p++; break;
        case 0:    putchar('\\');        break;   /* trailing backslash */
        default:   putchar('\\'); putchar(c); p++; break;
    }
    *pp = p;
}

/* Print S interpreting echo-style escapes (for %b). */
static void printf_b(const char *s) {
    const char *p = s;
    while (*p) {
        if (*p == '\\') { p++; printf_esc(&p, 1); }
        else            putchar(*p++);
    }
}

/* Parse a numeric argument. Supports a leading ' or " (value of next char)
 * and C-style bases (0x.., 0.., decimal). Flags printf_rc on bad input. */
static long long printf_sint(const char *s) {
    char *end;
    long long v;
    if (*s == '"' || *s == '\'') {
        v = (unsigned char)s[1];
        if (s[1] && s[2])
            fprintf(stderr, "printf: warning: %s: character(s) following "
                            "character constant have been ignored\n", s + 1);
        return v;
    }
    v = strtoll(s, &end, 0);
    if (end == s || *end) {
        fprintf(stderr, "printf: '%s': expected a numeric value\n", s);
        printf_rc = 1;
    }
    return v;
}
static unsigned long long printf_uint(const char *s) {
    char *end;
    unsigned long long v;
    if (*s == '"' || *s == '\'') {
        v = (unsigned char)s[1];
        if (s[1] && s[2])
            fprintf(stderr, "printf: warning: %s: character(s) following "
                            "character constant have been ignored\n", s + 1);
        return v;
    }
    v = strtoull(s, &end, 0);
    if (end == s || *end) {
        fprintf(stderr, "printf: '%s': expected a numeric value\n", s);
        printf_rc = 1;
    }
    return v;
}

/* Process a single conversion beginning at F (points at '%'). Consumes args
 * as needed (advancing *argi) and returns a pointer past the conversion. */
static const char *printf_conv(const char *f, char **args, int nargs, int *argi) {
    char spec[256];
    int si = 0;
    const char *p = f + 1;

    spec[si++] = '%';
    /* flags */
    while (*p && strchr("-+ 0#'", *p)) { if (si < 240) spec[si++] = *p; p++; }
    /* field width */
    if (*p == '*') {
        long long w = 0;
        if (*argi < nargs) { w = printf_sint(args[*argi]); (*argi)++; }
        si += snprintf(spec + si, sizeof spec - si, "%lld", w);
        if (si > 240) si = 240;
        p++;
    } else {
        while (isdigit((unsigned char)*p)) { if (si < 240) spec[si++] = *p; p++; }
    }
    /* precision */
    if (*p == '.') {
        if (si < 240) spec[si++] = '.';
        p++;
        if (*p == '*') {
            long long pr = 0;
            if (*argi < nargs) { pr = printf_sint(args[*argi]); (*argi)++; }
            si += snprintf(spec + si, sizeof spec - si, "%lld", pr);
            if (si > 240) si = 240;
            p++;
        } else {
            while (isdigit((unsigned char)*p)) { if (si < 240) spec[si++] = *p; p++; }
        }
    }
    /* drop any length modifiers the user typed */
    while (*p && strchr("hlLqjzt", *p)) p++;

    char conv = *p;
    if (conv) p++;

    const char *arg = (*argi < nargs) ? args[*argi] : NULL;

    switch (conv) {
        case 'd': case 'i': {
            long long v = arg ? printf_sint(arg) : 0;
            if (arg) (*argi)++;
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = 0;
            printf(spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = arg ? printf_uint(arg) : 0;
            if (arg) (*argi)++;
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
            printf(spec, v);
            break;
        }
        case 'c': {
            int ch = 0;
            if (arg) { ch = (unsigned char)arg[0]; (*argi)++; }
            spec[si++] = 'c'; spec[si] = 0;
            printf(spec, ch);
            break;
        }
        case 's': {
            const char *s = arg ? arg : "";
            if (arg) (*argi)++;
            spec[si++] = 's'; spec[si] = 0;
            printf(spec, s);
            break;
        }
        case 'b': {
            const char *s = arg ? arg : "";
            if (arg) (*argi)++;
            printf_b(s);
            break;
        }
        case 0:
            putchar('%');
            break;
        default:
            fprintf(stderr, "printf: %%%c: invalid conversion specification\n", conv);
            printf_rc = 1;
            break;
    }
    return p;
}

/* Run the format string once. Returns the number of args consumed. */
static int printf_run(const char *format, char **args, int nargs, int *argi) {
    int start = *argi;
    const char *f = format;
    while (*f) {
        if (*f == '\\') { f++; printf_esc(&f, 0); }
        else if (*f == '%') {
            if (f[1] == '%') { putchar('%'); f += 2; continue; }
            f = printf_conv(f, args, nargs, argi);
        } else putchar(*f++);
    }
    return *argi - start;
}

static int a_printf(int argc, char **argv) {
    printf_rc = 0;

    int base = 1;
    if (base < argc && strcmp(argv[base], "--") == 0) base++;
    if (base >= argc) {
        fprintf(stderr, "printf: missing operand\n"
                        "Try 'printf --help' for more information.\n");
        return 1;
    }

    const char *format = argv[base];
    char **args = argv + base + 1;
    int nargs = argc - base - 1;
    int argi = 0, used;

    do {
        used = printf_run(format, args, nargs, &argi);
    } while (used > 0 && argi < nargs);

    fflush(stdout);
    return printf_rc;
}

#pragma GCC diagnostic pop

/* ---- uname ---- */
static int a_uname(int argc, char **argv)
{
	struct utsname u;
	int f_s = 0, f_n = 0, f_r = 0, f_v = 0, f_m = 0, f_o = 0;
	int i;
	int first;

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (arg[0] == '-' && arg[1] != '\0') {
			char *p;
			for (p = arg + 1; *p; p++) {
				switch (*p) {
				case 's': f_s = 1; break;
				case 'n': f_n = 1; break;
				case 'r': f_r = 1; break;
				case 'v': f_v = 1; break;
				case 'm': f_m = 1; break;
				case 'o': f_o = 1; break;
				case 'a':
					f_s = f_n = f_r = f_v = f_m = f_o = 1;
					break;
				default:
					fprintf(stderr, "uname: invalid option -- '%c'\n", *p);
					return 1;
				}
			}
		} else {
			fprintf(stderr, "uname: extra operand '%s'\n", arg);
			return 1;
		}
	}

	if (!f_s && !f_n && !f_r && !f_v && !f_m && !f_o)
		f_s = 1;

	if (uname(&u) < 0) {
		fprintf(stderr, "uname: cannot get system name: %s\n", strerror(errno));
		return 1;
	}

	first = 1;
	if (f_s) { printf("%s%s", first ? "" : " ", u.sysname); first = 0; }
	if (f_n) { printf("%s%s", first ? "" : " ", u.nodename); first = 0; }
	if (f_r) { printf("%s%s", first ? "" : " ", u.release); first = 0; }
	if (f_v) { printf("%s%s", first ? "" : " ", u.version); first = 0; }
	if (f_m) { printf("%s%s", first ? "" : " ", u.machine); first = 0; }
	if (f_o) { printf("%s%s", first ? "" : " ", "GNU/Linux"); first = 0; }
	printf("\n");

	return 0;
}

/* ---- a_realpath ---- */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Lexical canonicalization (GNU -s / --no-symlinks): make absolute via cwd and
 * collapse "."/".." and redundant slashes WITHOUT touching the filesystem.
 * Result is always absolute. Returns 0 on success, -1 on error (errno set). */
static int realpath_lexical(const char *name, char *out, size_t outsz) {
    char in[PATH_MAX * 4];
    if (name[0] == '/') {
        if (strlen(name) >= sizeof in) { errno = ENAMETOOLONG; return -1; }
        strcpy(in, name);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return -1;
        if ((size_t)snprintf(in, sizeof in, "%s/%s", cwd, name) >= sizeof in) {
            errno = ENAMETOOLONG; return -1;
        }
    }
    size_t rlen = 0;
    out[0] = '\0';
    char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (rlen > 0 && out[rlen - 1] != '/') rlen--;
            if (rlen > 0) rlen--;          /* drop the '/' */
            out[rlen] = '\0';
            continue;
        }
        if (rlen + 1 + len + 1 > outsz) { errno = ENAMETOOLONG; return -1; }
        out[rlen++] = '/';
        memcpy(out + rlen, start, len);
        rlen += len;
        out[rlen] = '\0';
    }
    if (rlen == 0) { out[0] = '/'; out[1] = '\0'; }
    return 0;
}

/* Canonicalize allowing missing components (GNU -m): resolve symlinks for the
 * parts that exist, and treat everything from the first non-existent component
 * onward lexically. Returns 0 on success, -1 on error (errno set). */
static int realpath_missing(const char *name, char *out, size_t outsz) {
    char todo[PATH_MAX * 4];
    char res[PATH_MAX * 2];
    size_t reslen = 0;
    int links = 0, missing = 0;

    if (name[0] == '/') {
        if (strlen(name) >= sizeof todo) { errno = ENAMETOOLONG; return -1; }
        strcpy(todo, name);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return -1;
        if ((size_t)snprintf(todo, sizeof todo, "%s/%s", cwd, name) >= sizeof todo) {
            errno = ENAMETOOLONG; return -1;
        }
    }
    res[0] = '\0';

    char *p = todo;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (reslen > 0 && res[reslen - 1] != '/') reslen--;
            if (reslen > 0) reslen--;
            res[reslen] = '\0';
            continue;
        }

        char cand[PATH_MAX * 2];
        if (reslen + 1 + len + 1 > sizeof cand) { errno = ENAMETOOLONG; return -1; }
        memcpy(cand, res, reslen);
        cand[reslen] = '/';
        memcpy(cand + reslen + 1, start, len);
        cand[reslen + 1 + len] = '\0';

        if (!missing) {
            struct stat st;
            int r = lstat(cand, &st);
            if (r == 0 && S_ISLNK(st.st_mode)) {
                if (++links > 40) { errno = ELOOP; return -1; }
                char link[PATH_MAX];
                ssize_t n = readlink(cand, link, sizeof link - 1);
                if (n < 0) return -1;
                link[n] = '\0';
                char next[PATH_MAX * 4];
                if (link[0] == '/') { reslen = 0; res[0] = '\0'; }
                if ((size_t)snprintf(next, sizeof next, "%s%s", link, p) >= sizeof next) {
                    errno = ENAMETOOLONG; return -1;
                }
                strcpy(todo, next);
                p = todo;
                continue;
            }
            if (r != 0) missing = 1;      /* from here on treat lexically */
        }

        if (reslen + 1 + len + 1 > sizeof res) { errno = ENAMETOOLONG; return -1; }
        res[reslen++] = '/';
        memcpy(res + reslen, start, len);
        reslen += len;
        res[reslen] = '\0';
    }
    if (reslen == 0) { res[0] = '/'; res[1] = '\0'; reslen = 1; }
    if (reslen + 1 > outsz) { errno = ENAMETOOLONG; return -1; }
    strcpy(out, res);
    return 0;
}

static int a_realpath(int argc, char **argv) {
    int eflag = 0, mflag = 0, sflag = 0, qflag = 0, zflag = 0;
    (void)eflag;   /* default and -e both require the whole path to exist */
    char **ops = malloc(sizeof(char *) * (size_t)(argc > 0 ? argc : 1));
    if (!ops) { fprintf(stderr, "%s: out of memory\n", argv[0]); return 1; }
    int nops = 0, endopts = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!endopts && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-') {
                if (a[2] == '\0') { endopts = 1; continue; }
                if      (!strcmp(a, "--no-symlinks") || !strcmp(a, "--strip")) sflag = 1;
                else if (!strcmp(a, "--canonicalize-existing")) eflag = 1;
                else if (!strcmp(a, "--canonicalize-missing"))  mflag = 1;
                else if (!strcmp(a, "--quiet") || !strcmp(a, "--silent")) qflag = 1;
                else if (!strcmp(a, "--zero")) zflag = 1;
                else if (!strcmp(a, "--help")) {
                    printf("Usage: %s [OPTION]... FILE...\n"
                           "Print the resolved absolute file name.\n\n"
                           "  -e  all components of the path must exist\n"
                           "  -m  no path components need exist or be a directory\n"
                           "  -s  don't expand symlinks\n"
                           "  -q  suppress most error messages\n"
                           "  -z  end each output line with NUL, not newline\n",
                           argv[0]);
                    free(ops); return 0;
                }
                else if (!strcmp(a, "--version")) {
                    printf("realpath (verkbox)\n"); free(ops); return 0;
                }
                else {
                    fprintf(stderr, "%s: unrecognized option '%s'\n"
                            "Try '%s --help' for more information.\n",
                            argv[0], a, argv[0]);
                    free(ops); return 1;
                }
            } else {
                for (char *c = a + 1; *c; c++) {
                    switch (*c) {
                        case 'e': eflag = 1; break;
                        case 'm': mflag = 1; break;
                        case 's': sflag = 1; break;
                        case 'q': qflag = 1; break;
                        case 'z': zflag = 1; break;
                        default:
                            fprintf(stderr, "%s: invalid option -- '%c'\n"
                                    "Try '%s --help' for more information.\n",
                                    argv[0], *c, argv[0]);
                            free(ops); return 1;
                    }
                }
            }
        } else {
            ops[nops++] = a;
        }
    }

    if (nops == 0) {
        fprintf(stderr, "%s: missing operand\n"
                "Try '%s --help' for more information.\n", argv[0], argv[0]);
        free(ops); return 1;
    }

    int rc = 0;
    char sep = zflag ? '\0' : '\n';
    char buf[PATH_MAX * 2];

    for (int k = 0; k < nops; k++) {
        const char *path = ops[k];
        int ok;
        errno = 0;
        if (path[0] == '\0') { errno = ENOENT; ok = 0; }
        else if (sflag)      ok = (realpath_lexical(path, buf, sizeof buf) == 0);
        else if (mflag)      ok = (realpath_missing(path, buf, sizeof buf) == 0);
        else                 ok = (realpath(path, buf) != NULL);

        if (ok) {
            fputs(buf, stdout);
            putchar(sep);
        } else {
            rc = 1;
            if (!qflag) {
                const char *disp = path[0] ? path : "''";
                fprintf(stderr, "%s: %s: %s\n", argv[0], disp, strerror(errno));
            }
        }
    }
    free(ops);
    return rc;
}

/* ---- env ---- */
static int a_env(int argc, char **argv) {
    extern char **environ;
    int ignore = 0;
    char *unset[argc > 0 ? argc : 1];   /* at most argc-1 names to unset */
    int nunset = 0;
    int i = 1;

    /* ---- option parsing: options precede operands (getopt '+' style) ------ */
    while (i < argc) {
        char *a = argv[i];
        if (a[0] != '-') break;                                 /* operand */
        if (a[1] == '\0') { ignore = 1; i++; continue; }        /* "-" => -i */
        if (a[1] == '-' && a[2] == '\0') { i++; break; }        /* "--" ends */
        if (a[1] == '-') {                                      /* long option */
            if (strcmp(a, "--ignore-environment") == 0) { ignore = 1; i++; continue; }
            if (strncmp(a, "--unset=", 8) == 0) { unset[nunset++] = a + 8; i++; continue; }
            if (strcmp(a, "--unset") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "env: option '--unset' requires an argument\n");
                    return 125;
                }
                unset[nunset++] = argv[i + 1]; i += 2; continue;
            }
            fprintf(stderr, "env: unrecognized option '%s'\n", a);
            return 125;
        }
        /* short-option cluster, e.g. -i, -iu NAME, -uNAME */
        {
            char *p = a + 1;
            int consumed_next = 0;
            int bad = 0;
            while (*p) {
                if (*p == 'i') { ignore = 1; p++; continue; }
                if (*p == 'u') {
                    p++;
                    if (*p) { unset[nunset++] = p; }
                    else {
                        if (i + 1 >= argc) {
                            fprintf(stderr, "env: option requires an argument -- 'u'\n");
                            return 125;
                        }
                        unset[nunset++] = argv[i + 1]; consumed_next = 1;
                    }
                    break;                            /* rest of arg is the name */
                }
                fprintf(stderr, "env: invalid option -- '%c'\n", *p);
                bad = 1; break;
            }
            if (bad) return 125;
            i += consumed_next ? 2 : 1;
        }
    }

    /* ---- apply environment modifications in order ------------------------- */
    if (ignore) clearenv();
    for (int k = 0; k < nunset; k++) unsetenv(unset[k]);

    /* leading operands containing '=' are NAME=VALUE assignments */
    while (i < argc && strchr(argv[i], '=') != NULL) {
        char *a = argv[i];
        char *eq = strchr(a, '=');
        size_t nlen = (size_t)(eq - a);
        char *nm = (char *)malloc(nlen + 1);
        if (nm) {
            memcpy(nm, a, nlen);
            nm[nlen] = '\0';
            setenv(nm, eq + 1, 1);
            free(nm);
        }
        i++;
    }

    /* ---- run command, or print the environment --------------------------- */
    if (i < argc) {
        execvp(argv[i], &argv[i]);
        int ec = (errno == ENOENT) ? 127 : 126;   /* only reached on failure */
        fprintf(stderr, "env: '%s': %s\n", argv[i], strerror(errno));
        return ec;
    }

    for (char **e = environ; e && *e; ++e) {
        fputs(*e, stdout);
        putchar('\n');
    }
    return 0;
}

/* ---- sleep ---- */
static int a_sleep(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "sleep: missing operand\n");
        return 1;
    }

    double total = 0.0;
    int ok = 1;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        char *endp;
        errno = 0;
        double s = strtod(arg, &endp);
        double mult = 1.0;
        int good = 1;

        if (endp == arg || s < 0.0) {
            /* no numeric conversion, or a negative interval */
            good = 0;
        } else if (endp[0] != '\0' && endp[1] != '\0') {
            /* more than a single trailing suffix character */
            good = 0;
        } else {
            switch (endp[0]) {
                case '\0':
                case 's': mult = 1.0;     break;
                case 'm': mult = 60.0;    break;
                case 'h': mult = 3600.0;  break;
                case 'd': mult = 86400.0; break;
                default:  good = 0;       break;
            }
        }

        if (!good) {
            fprintf(stderr, "sleep: invalid time interval '%s'\n", arg);
            ok = 0;
        } else {
            total += s * mult;
        }
    }

    if (!ok)
        return 1;

    if (total <= 0.0)
        return 0;

    struct timespec ts;
    ts.tv_sec = (time_t) total;
    double frac = total - (double) ts.tv_sec;
    if (frac < 0.0) frac = 0.0;
    long nsec = (long) (frac * 1e9);
    if (nsec < 0L) nsec = 0L;
    if (nsec > 999999999L) nsec = 999999999L;
    ts.tv_nsec = nsec;

    /* nanosleep updates ts with the remaining time when interrupted */
    while (nanosleep(&ts, &ts) != 0) {
        if (errno == EINTR)
            continue;
        fprintf(stderr, "sleep: cannot read realtime clock: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}


/* ------------------------------------------------------------ dispatch ---- */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet applets[] = {
    {"true",a_true},{"false",a_false},{"echo",a_echo},{"cat",a_cat},{"pwd",a_pwd},
    {"whoami",a_whoami},{"nproc",a_nproc},{"yes",a_yes},{"basename",a_basename},
    {"dirname",a_dirname},{"head",a_head},{"wc",a_wc},{"seq",a_seq},
    {"ls",a_ls},{"tail",a_tail},{"cut",a_cut},{"rev",a_rev},{"uniq",a_uniq},{"mkdir",a_mkdir},{"rmdir",a_rmdir},{"rm",a_rm},{"cp",a_cp},{"mv",a_mv},{"ln",a_ln},{"touch",a_touch},
    {"sort",a_sort},{"tr",a_tr},{"nl",a_nl},{"tac",a_tac},{"tee",a_tee},{"fold",a_fold},{"comm",a_comm},{"paste",a_paste},
    {"grep",a_grep},{"chmod",a_chmod},{"du",a_du},{"printf",a_printf},{"uname",a_uname},{"realpath",a_realpath},{"env",a_env},{"sleep",a_sleep},{NULL,NULL}
};

static int list_applets(void) {
    printf("verkbox %s — VerkOS core utilities\napplets:", VERKBOX_VERSION);
    for (const struct applet *a = applets; a->name; a++) printf(" %s", a->name);
    putchar('\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *b = strrchr(argv[0], '/'); b = b ? b + 1 : argv[0];
    if (!strcmp(b, "verkbox")) {
        if (argc < 2) return list_applets();
        if (!strcmp(argv[1], "--list")) return list_applets();
        b = argv[1]; argc--; argv++;                      /* shift to applet */
    }
    for (const struct applet *a = applets; a->name; a++)
        if (!strcmp(b, a->name)) return a->fn(argc, argv);
    fprintf(stderr, "verkbox: unknown applet '%s'\n", b);
    return 127;
}
