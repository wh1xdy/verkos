/* vlogin — VerkOS' own account tools, replacing shadow.
 *
 * One multi-call binary, BusyBox-style:
 *
 *   login [-f USER] [-h HOST] [USER]   authenticate and start a session
 *   su [-|-l] [-c CMD] [USER]          become another user
 *   passwd [USER]                      change a password
 *   chpasswd                           set passwords from USER:PASSWORD lines
 *   useradd [-m] [-u UID] [-g GRP] [-G GRPS] [-d DIR] [-s SHELL] [-c GECOS] NAME
 *   usermod [-aG GRPS] [-s SH] NAME    change an existing account
 *   userdel [-r] NAME                  remove an account (-r: and its home)
 *   groupadd [-g GID] NAME
 *   groupdel NAME
 *   pwconv                             move passwords from /etc/passwd to /etc/shadow
 *   nologin                            refuse a login politely (a login shell)
 *
 * Hashing is libxcrypt's, deliberately: crypt_gensalt picks the strongest
 * scheme it supports (yescrypt) and crypt() verifies every scheme it knows, so
 * hashes written by shadow keep working and nobody has to trust a hand-rolled
 * password hash. See docs/DECISIONS.md D-013.
 *
 * Reads use libc's getpwnam/getspnam/getgrnam so nsswitch still applies; writes
 * rewrite the file atomically under lckpwdf(), the same lock shadow takes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <crypt.h>
#include <termios.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utmpx.h>
#include <sys/time.h>

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"
#define GROUP_FILE  "/etc/group"
#define SKEL_DIR    "/etc/skel"
#define WTMP_FILE   "/var/log/wtmp"
#define LOGIN_TRIES 3
#define FIRST_UID   1000
#define FIRST_GID   1000

static const char *prog = "vlogin";

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* days since the epoch — the unit /etc/shadow counts ages in */
static long today_days(void) { return (long)(time(NULL) / 86400); }

/* ---- reading a password without echoing it -------------------------------- */
static struct termios saved_tio;
static int tio_saved;
static void tio_restore(void) { if (tio_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio); }
static void on_signal(int sig) { tio_restore(); _exit(128 + sig); }

/* Returns a static buffer, or NULL on EOF. */
static char *ask_password(const char *prompt) {
    static char buf[256];
    struct termios t;
    int have_tty = isatty(STDIN_FILENO);

    fputs(prompt, stderr);
    fflush(stderr);

    if (have_tty && tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
        t = saved_tio;
        t.c_lflag &= ~(unsigned)ECHO;
        t.c_lflag |= ECHONL;
        tio_saved = 1;
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    }

    char *r = fgets(buf, sizeof buf, stdin);

    if (tio_saved) { tio_restore(); tio_saved = 0; signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); }
    if (!have_tty) fputc('\n', stderr);
    if (!r) return NULL;
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

/* ---- password verification ------------------------------------------------
 * The stored hash carries its own scheme and salt; crypt() re-derives with the
 * same parameters, so this one comparison covers every scheme libxcrypt knows,
 * including whatever shadow wrote before us. */
static int password_ok(const char *hash, const char *given) {
    if (!hash) return 0;
    if (!*hash) return 1;                       /* no password set */
    if (hash[0] == '!' || hash[0] == '*') return 0;   /* locked */
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *got = crypt_r(given, hash, &cd);
    if (!got) return 0;
    /* Length-independent compare; the hash is not secret but the habit is right. */
    size_t a = strlen(got), b = strlen(hash);
    unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < a && i < b; i++) diff |= (unsigned)(got[i] ^ hash[i]);
    return diff == 0;
}

/* The authoritative hash for a user: /etc/shadow if the passwd field is "x". */
static const char *stored_hash(const struct passwd *pw) {
    if (pw->pw_passwd && strcmp(pw->pw_passwd, "x") != 0) return pw->pw_passwd;
    struct spwd *sp = getspnam(pw->pw_name);
    return sp ? sp->sp_pwdp : NULL;
}

static char *hash_password(const char *plain) {
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    /* NULL prefix: libxcrypt's strongest available scheme, its own parameters. */
    if (!crypt_gensalt_rn(NULL, 0, NULL, 0, salt, sizeof salt))
        die("could not generate a salt: %s", strerror(errno));
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    char *h = crypt_r(plain, salt, &cd);
    if (!h || h[0] == '*') die("hashing failed");
    return strdup(h);
}

/* ---- atomic rewrite of a database file ------------------------------------
 * Write a sibling temp file, fsync it, rename over the original: a reader
 * either sees the whole old file or the whole new one, and a crash mid-write
 * cannot leave /etc/shadow truncated — which would lock everyone out. */
struct dbfile { FILE *f; char tmp[256]; char final[256]; mode_t mode; };

static int db_begin(struct dbfile *d, const char *path, mode_t mode) {
    snprintf(d->final, sizeof d->final, "%s", path);
    snprintf(d->tmp, sizeof d->tmp, "%s.vlogin.%d", path, (int)getpid());
    d->mode = mode;
    int fd = open(d->tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return -1;
    d->f = fdopen(fd, "w");
    if (!d->f) { close(fd); unlink(d->tmp); return -1; }
    return 0;
}
static int db_commit(struct dbfile *d) {
    if (fflush(d->f) || fsync(fileno(d->f))) { fclose(d->f); unlink(d->tmp); return -1; }
    if (fchmod(fileno(d->f), d->mode)) { fclose(d->f); unlink(d->tmp); return -1; }
    fclose(d->f);
    if (rename(d->tmp, d->final)) { unlink(d->tmp); return -1; }
    return 0;
}

static void lock_db(void) {
    if (lckpwdf() != 0) die("%s is busy (another account tool is running)", PASSWD_FILE);
}
static void unlock_db(void) { ulckpwdf(); }

/* Rewrite <path>, replacing the line whose first colon-field is <key>.
 * repl == NULL deletes the line. Returns 1 if the key was found. */
static int db_replace_line(const char *path, mode_t mode, const char *key, const char *repl) {
    FILE *in = fopen(path, "r");
    if (!in) return 0;
    struct dbfile out;
    if (db_begin(&out, path, mode)) { fclose(in); die("cannot write %s: %s", path, strerror(errno)); }

    char line[4096];
    size_t klen = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, key, klen) && line[klen] == ':') {
            found = 1;
            if (repl) fprintf(out.f, "%s\n", repl);
            continue;
        }
        fputs(line, out.f);
    }
    fclose(in);
    if (db_commit(&out)) die("cannot replace %s: %s", path, strerror(errno));
    return found;
}

static void db_append_line(const char *path, mode_t mode, const char *line) {
    /* Copy-then-append rather than O_APPEND, so the file is replaced atomically
     * and keeps its mode even if it did not exist before. */
    struct dbfile out;
    if (db_begin(&out, path, mode)) die("cannot write %s: %s", path, strerror(errno));
    FILE *in = fopen(path, "r");
    if (in) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out.f);
        fclose(in);
    }
    fprintf(out.f, "%s\n", line);
    if (db_commit(&out)) die("cannot update %s: %s", path, strerror(errno));
}

/* ---- session accounting (so `who` and `last` keep working) ---------------- */
static void record_login(const char *user, const char *tty, const char *host) {
    struct utmpx u;
    memset(&u, 0, sizeof u);
    u.ut_type = USER_PROCESS;
    u.ut_pid = getpid();
    snprintf(u.ut_line, sizeof u.ut_line, "%s", tty);
    snprintf(u.ut_id, sizeof u.ut_id, "%s", strlen(tty) > 3 ? tty + strlen(tty) - 4 : tty);
    snprintf(u.ut_user, sizeof u.ut_user, "%s", user);
    if (host) snprintf(u.ut_host, sizeof u.ut_host, "%s", host);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    u.ut_tv.tv_sec = (int32_t)tv.tv_sec;
    u.ut_tv.tv_usec = (int32_t)tv.tv_usec;
    setutxent();
    pututxline(&u);
    endutxent();
    updwtmpx(WTMP_FILE, &u);
}

/* ---- dropping privilege ---------------------------------------------------
 * Order matters and is the classic way to get this wrong: setgid and the
 * supplementary groups must happen while still root, because after setuid the
 * process can no longer change them. Every step is checked — an ignored failure
 * here means a shell that kept privileges it should not have. */
static void become_user(const struct passwd *pw) {
    if (setgid(pw->pw_gid) != 0)
        die("setgid(%u) failed: %s", (unsigned)pw->pw_gid, strerror(errno));
    if (initgroups(pw->pw_name, pw->pw_gid) != 0)
        die("initgroups failed: %s", strerror(errno));
    if (setuid(pw->pw_uid) != 0)
        die("setuid(%u) failed: %s", (unsigned)pw->pw_uid, strerror(errno));
    /* Belt and braces: prove the privileges are actually gone. */
    if (getuid() != pw->pw_uid || geteuid() != pw->pw_uid)
        die("failed to drop to uid %u", (unsigned)pw->pw_uid);
    if (pw->pw_uid != 0 && setuid(0) == 0)
        die("privileges were not dropped");
}

static void setup_environment(const struct passwd *pw, int login_shell) {
    const char *term = getenv("TERM");
    const char *tz   = getenv("TZ");
    if (login_shell) {
        clearenv();
        if (term) setenv("TERM", term, 1);
        if (tz) setenv("TZ", tz, 1);
    }
    setenv("HOME", pw->pw_dir, 1);
    setenv("SHELL", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh", 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("PATH", pw->pw_uid == 0 ? "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin"
                                   : "/usr/local/bin:/usr/bin", 1);
}

static void exec_shell(const struct passwd *pw, int login_shell, const char *command) {
    const char *shell = pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    if (chdir(pw->pw_dir) != 0) {
        fprintf(stderr, "%s: no home directory %s, using /\n", prog, pw->pw_dir);
        if (chdir("/") != 0) die("chdir(/) failed: %s", strerror(errno));
        setenv("HOME", "/", 1);
    }
    if (command) {
        execl(shell, shell, "-c", command, (char *)NULL);
    } else if (login_shell) {
        /* A leading '-' in argv[0] is how a shell is told it is a login shell. */
        const char *base = strrchr(shell, '/');
        char arg0[256];
        snprintf(arg0, sizeof arg0, "-%s", base ? base + 1 : shell);
        execl(shell, arg0, (char *)NULL);
    } else {
        execl(shell, shell, (char *)NULL);
    }
    die("cannot run %s: %s", shell, strerror(errno));
}

/* ---- login ---------------------------------------------------------------- */
static int cmd_login(int argc, char **argv) {
    const char *user = NULL, *host = NULL;
    int noauth = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) { noauth = 1; user = argv[++i]; }
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "-p")) continue;      /* preserve environment: we do anyway */
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else user = argv[i];
    }
    /* -f is a request to skip authentication entirely, so only root may ask. */
    if (noauth && getuid() != 0) die("only root can use -f");

    const char *tty = ttyname(STDIN_FILENO);
    tty = tty ? (strncmp(tty, "/dev/", 5) ? tty : tty + 5) : "console";

    char namebuf[256];
    for (int try = 0; try < LOGIN_TRIES; try++) {
        if (!user) {
            char hostname[64] = "";
            gethostname(hostname, sizeof hostname - 1);
            fprintf(stderr, "%s login: ", hostname);
            fflush(stderr);
            if (!fgets(namebuf, sizeof namebuf, stdin)) return 1;
            namebuf[strcspn(namebuf, "\n")] = 0;
            if (!namebuf[0]) { user = NULL; try--; continue; }
            user = namebuf;
        }

        struct passwd *pw = getpwnam(user);
        /* Ask for a password even when the user does not exist, so that timing
         * and prompts do not reveal which names are real. */
        const char *hash = pw ? stored_hash(pw) : "!";

        if (!noauth) {
            char *given = ask_password("Password: ");
            if (!given) return 1;
            int good = pw && password_ok(hash, given);
            memset(given, 0, strlen(given));
            if (!good) {
                sleep(2);
                fprintf(stderr, "Login incorrect\n\n");
                user = NULL;
                continue;
            }
        }
        if (!pw) { sleep(2); fprintf(stderr, "Login incorrect\n\n"); user = NULL; continue; }

        /* The console belongs to whoever is logged in on it. */
        char ttypath[256];
        snprintf(ttypath, sizeof ttypath, "/dev/%s", tty);
        if (chown(ttypath, pw->pw_uid, pw->pw_gid) == 0) chmod(ttypath, 0600);

        record_login(pw->pw_name, tty, host);
        setup_environment(pw, 1);
        become_user(pw);
        exec_shell(pw, 1, NULL);
    }
    fprintf(stderr, "Maximum number of tries exceeded\n");
    return 1;
}

/* ---- su ------------------------------------------------------------------- */
static int cmd_su(int argc, char **argv) {
    int login_shell = 0;
    const char *command = NULL, *user = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l") || !strcmp(argv[i], "--login"))
            login_shell = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) command = argv[++i];
        else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "-p")) continue;
        else if (argv[i][0] == '-' && argv[i][1]) die("unknown option %s", argv[i]);
        else user = argv[i];
    }
    if (!user) user = "root";

    struct passwd *pw = getpwnam(user);
    if (!pw) die("unknown user %s", user);

    /* root may become anyone without proving it; everyone else authenticates as
     * the *target*, which is what makes su usable with only that password. */
    if (getuid() != 0) {
        const char *hash = stored_hash(pw);
        char *given = ask_password("Password: ");
        if (!given) return 1;
        int good = password_ok(hash, given);
        memset(given, 0, strlen(given));
        if (!good) { sleep(2); fprintf(stderr, "su: Authentication failure\n"); return 1; }
    }

    setup_environment(pw, login_shell);
    become_user(pw);
    exec_shell(pw, login_shell, command);
    return 1;
}

/* ---- passwd --------------------------------------------------------------- */
/* Write a new hash into /etc/shadow (creating the entry if the account has its
 * password still in /etc/passwd). */
static void set_password(const char *name, const char *hash) {
    struct passwd *pw = getpwnam(name);
    if (!pw) die("unknown user %s", name);

    lock_db();
    struct spwd *sp = getspnam(name);
    char line[4096];
    if (sp) {
        /* Keep the ageing fields the account already had; only the hash and the
         * "last changed" day are ours to set. Empty means "unset", so an unset
         * field must stay empty rather than become -1. */
        char inact[32] = "", expire[32] = "";
        if (sp->sp_inact >= 0)  snprintf(inact,  sizeof inact,  "%ld", (long)sp->sp_inact);
        if (sp->sp_expire >= 0) snprintf(expire, sizeof expire, "%ld", (long)sp->sp_expire);
        snprintf(line, sizeof line, "%s:%s:%ld:%ld:%ld:%ld:%s:%s:",
                 name, hash, today_days(),
                 (long)(sp->sp_min >= 0 ? sp->sp_min : 0),
                 (long)(sp->sp_max >= 0 ? sp->sp_max : 99999),
                 (long)(sp->sp_warn >= 0 ? sp->sp_warn : 7),
                 inact, expire);
        if (!db_replace_line(SHADOW_FILE, 0640, name, line))
            db_append_line(SHADOW_FILE, 0640, line);
    } else {
        snprintf(line, sizeof line, "%s:%s:%ld:0:99999:7:::", name, hash, today_days());
        db_append_line(SHADOW_FILE, 0640, line);
    }
    /* Point /etc/passwd at the shadow file if it still carried a hash. */
    if (pw->pw_passwd && strcmp(pw->pw_passwd, "x")) {
        snprintf(line, sizeof line, "%s:x:%u:%u:%s:%s:%s", pw->pw_name,
                 (unsigned)pw->pw_uid, (unsigned)pw->pw_gid,
                 pw->pw_gecos, pw->pw_dir, pw->pw_shell);
        db_replace_line(PASSWD_FILE, 0644, name, line);
    }
    unlock_db();
}

static int cmd_passwd(int argc, char **argv) {
    const char *target = NULL;
    int lock = 0, unlock = 0, del = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) lock = 1;
        else if (!strcmp(argv[i], "-u")) unlock = 1;
        else if (!strcmp(argv[i], "-d")) del = 1;
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else target = argv[i];
    }

    struct passwd *self = getpwuid(getuid());
    if (!target) target = self ? self->pw_name : NULL;
    if (!target) die("cannot determine who you are");
    struct passwd *pw = getpwnam(target);
    if (!pw) die("unknown user %s", target);

    int am_root = (getuid() == 0);
    if (!am_root && (!self || strcmp(self->pw_name, target)))
        die("you may only change your own password");
    if (!am_root && (lock || unlock || del))
        die("only root can lock, unlock or clear a password");

    if (lock || unlock || del) {
        const char *cur = stored_hash(pw);
        char newhash[512];
        if (del) newhash[0] = 0;
        else if (lock) snprintf(newhash, sizeof newhash, "!%s", cur && *cur ? cur : "");
        else snprintf(newhash, sizeof newhash, "%s", cur && cur[0] == '!' ? cur + 1 : (cur ? cur : ""));
        set_password(target, newhash);
        printf("passwd: password %s for %s\n",
               del ? "cleared" : lock ? "locked" : "unlocked", target);
        return 0;
    }

    /* Changing your own password means proving you know the current one. */
    if (!am_root) {
        const char *cur = stored_hash(pw);
        if (cur && *cur && cur[0] != '!' && cur[0] != '*') {
            char *old = ask_password("Current password: ");
            if (!old) return 1;
            int good = password_ok(cur, old);
            memset(old, 0, strlen(old));
            if (!good) { sleep(2); fprintf(stderr, "passwd: Authentication failure\n"); return 1; }
        }
    }

    char first[256];
    char *p1 = ask_password("New password: ");
    if (!p1) return 1;
    snprintf(first, sizeof first, "%s", p1);
    memset(p1, 0, strlen(p1));
    if (strlen(first) < 6 && !am_root) {
        memset(first, 0, sizeof first);
        die("password must be at least 6 characters");
    }
    char *p2 = ask_password("Retype new password: ");
    if (!p2) { memset(first, 0, sizeof first); return 1; }
    int same = !strcmp(first, p2);
    memset(p2, 0, strlen(p2));
    if (!same) {
        memset(first, 0, sizeof first);
        fprintf(stderr, "passwd: passwords do not match\n");
        return 1;
    }

    char *hash = hash_password(first);
    memset(first, 0, sizeof first);
    set_password(target, hash);
    printf("passwd: password updated successfully for %s\n", target);
    return 0;
}

/* chpasswd: USER:PASSWORD lines on stdin. The build uses this to set the
 * initial root password, so it has to exist before shadow can be removed. */
static int cmd_chpasswd(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getuid() != 0) die("only root can run chpasswd");
    char line[1024];
    int rc = 0, n = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        char *colon = strchr(line, ':');
        if (!colon) { fprintf(stderr, "%s: missing ':' in input\n", prog); rc = 1; continue; }
        *colon = 0;
        if (!getpwnam(line)) { fprintf(stderr, "%s: unknown user %s\n", prog, line); rc = 1; continue; }
        char *hash = hash_password(colon + 1);
        set_password(line, hash);
        free(hash);
        memset(colon + 1, 0, strlen(colon + 1));
        n++;
    }
    memset(line, 0, sizeof line);
    if (n == 0 && rc == 0) fprintf(stderr, "%s: no passwords changed\n", prog);
    return rc;
}

/* pwconv: move any password still living in /etc/passwd into /etc/shadow. */
static int cmd_pwconv(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getuid() != 0) die("only root can run pwconv");
    int moved = 0;
    /* Collect first: set_password rewrites the files we would be iterating. */
    char names[512][64];
    char hashes[512][256];
    int n = 0;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) && n < 512) {
        if (!pw->pw_passwd || !strcmp(pw->pw_passwd, "x")) continue;
        if (getspnam(pw->pw_name)) continue;
        snprintf(names[n], sizeof names[n], "%s", pw->pw_name);
        snprintf(hashes[n], sizeof hashes[n], "%s", pw->pw_passwd);
        n++;
    }
    endpwent();
    for (int i = 0; i < n; i++) { set_password(names[i], hashes[i]); moved++; }
    /* Accounts with no shadow entry at all get a locked one, so /etc/shadow is
     * complete and nothing is accidentally passwordless. */
    setpwent();
    while ((pw = getpwent())) {
        if (getspnam(pw->pw_name)) continue;
        char line[512];
        snprintf(line, sizeof line, "%s:!:%ld:0:99999:7:::", pw->pw_name, today_days());
        lock_db(); db_append_line(SHADOW_FILE, 0640, line); unlock_db();
    }
    endpwent();
    if (moved) printf("pwconv: moved %d password(s) into %s\n", moved, SHADOW_FILE);
    return 0;
}

/* ---- account management ---------------------------------------------------
 * These all refuse unless the caller is really root, so the setuid bit on the
 * binary buys them nothing; the absolute paths below are still there so a
 * hostile PATH cannot decide what "cp" and "rm" mean. */
static uid_t next_free_uid(void) {
    uid_t best = FIRST_UID;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()))
        if (pw->pw_uid >= best && pw->pw_uid < 60000) best = pw->pw_uid + 1;
    endpwent();
    return best;
}
static gid_t next_free_gid(void) {
    gid_t best = FIRST_GID;
    setgrent();
    struct group *gr;
    while ((gr = getgrent()))
        if (gr->gr_gid >= best && gr->gr_gid < 60000) best = gr->gr_gid + 1;
    endgrent();
    return best;
}

static void add_group(const char *name, gid_t gid) {
    char line[512];
    snprintf(line, sizeof line, "%s:x:%u:", name, (unsigned)gid);
    db_append_line(GROUP_FILE, 0644, line);
}

/* Append <user> to the member list of an existing group. */
static void add_to_group(const char *group, const char *user) {
    struct group *gr = getgrnam(group);
    if (!gr) die("unknown group %s", group);
    char members[2048] = "";
    for (char **m = gr->gr_mem; m && *m; m++) {
        if (!strcmp(*m, user)) return;                 /* already a member */
        if (members[0]) strncat(members, ",", sizeof members - strlen(members) - 1);
        strncat(members, *m, sizeof members - strlen(members) - 1);
    }
    if (members[0]) strncat(members, ",", sizeof members - strlen(members) - 1);
    strncat(members, user, sizeof members - strlen(members) - 1);
    char line[4096];
    snprintf(line, sizeof line, "%s:x:%u:%s", gr->gr_name, (unsigned)gr->gr_gid, members);
    db_replace_line(GROUP_FILE, 0644, gr->gr_name, line);
}

static void copy_skel(const char *home, uid_t uid, gid_t gid) {
    if (mkdir(home, 0755) != 0 && errno != EEXIST)
        die("cannot create %s: %s", home, strerror(errno));
    /* /etc/skel is a flat directory of dotfiles in practice; copy its regular
     * files rather than growing a recursive copier here. */
    char cmd[1024];
    struct stat st;
    if (stat(SKEL_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(cmd, sizeof cmd, "/usr/bin/cp -r %s/. %s/ 2>/dev/null", SKEL_DIR, home);
        if (system(cmd) != 0) { /* an empty or missing skel is not an error */ }
    }
    if (chown(home, uid, gid) != 0)
        fprintf(stderr, "%s: cannot chown %s: %s\n", prog, home, strerror(errno));
    chmod(home, 0755);
}

static int cmd_useradd(int argc, char **argv) {
    if (getuid() != 0) die("only root can add users");
    const char *name = NULL, *home = NULL, *shell = "/bin/bash", *gecos = "";
    const char *group = NULL, *groups = NULL;
    long uid = -1;
    int make_home = 0, system_acct = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) make_home = 1;
        else if (!strcmp(argv[i], "-M")) make_home = 0;
        else if (!strcmp(argv[i], "-r")) system_acct = 1;
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) uid = atol(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) group = argv[++i];
        else if (!strcmp(argv[i], "-G") && i + 1 < argc) groups = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) gecos = argv[++i];
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else name = argv[i];
    }
    if (!name) die("usage: useradd [-m] [-u UID] [-g GROUP] [-G GROUPS] [-d DIR] [-s SHELL] [-c GECOS] NAME");
    if (getpwnam(name)) die("user %s already exists", name);
    if (strchr(name, ':')) die("a user name cannot contain ':'");

    char homebuf[256];
    if (!home) { snprintf(homebuf, sizeof homebuf, "/home/%s", name); home = homebuf; }

    lock_db();
    uid_t u = uid >= 0 ? (uid_t)uid : next_free_uid();
    if (system_acct && uid < 0) u = 100;            /* a plain low uid is enough here */
    gid_t g;
    if (group) {
        struct group *gr = getgrnam(group);
        if (!gr) die("unknown group %s", group);
        g = gr->gr_gid;
    } else {
        g = next_free_gid();
        add_group(name, g);                         /* the usual per-user group */
    }

    char line[1024];
    snprintf(line, sizeof line, "%s:x:%u:%u:%s:%s:%s",
             name, (unsigned)u, (unsigned)g, gecos, home, shell);
    db_append_line(PASSWD_FILE, 0644, line);
    /* '!' means "no password set": the account exists but cannot be logged into
     * until passwd is run. Never leave a new account passwordless. */
    snprintf(line, sizeof line, "%s:!:%ld:0:99999:7:::", name, today_days());
    db_append_line(SHADOW_FILE, 0640, line);
    unlock_db();

    if (groups) {
        char buf[512]; snprintf(buf, sizeof buf, "%s", groups);
        char *save = NULL;
        for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            lock_db(); add_to_group(t, name); unlock_db();
        }
    }
    if (make_home) copy_skel(home, u, g);
    return 0;
}

static int cmd_usermod(int argc, char **argv) {
    if (getuid() != 0) die("only root can modify users");
    const char *name = NULL, *shell = NULL, *home = NULL, *groups = NULL, *group = NULL;
    long uid = -1;
    int append = 0, lock = 0, unlock = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) append = 1;
        else if (!strcmp(argv[i], "-aG") && i + 1 < argc) { append = 1; groups = argv[++i]; }
        else if (!strcmp(argv[i], "-G") && i + 1 < argc) groups = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) group = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) uid = atol(argv[++i]);
        else if (!strcmp(argv[i], "-L")) lock = 1;
        else if (!strcmp(argv[i], "-U")) unlock = 1;
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else name = argv[i];
    }
    if (!name) die("usage: usermod [-aG GROUPS] [-s SHELL] [-d DIR] [-u UID] [-g GROUP] [-L|-U] NAME");
    struct passwd *pw = getpwnam(name);
    if (!pw) die("unknown user %s", name);

    /* Copy out of the static getpwnam buffer before anything else touches it. */
    char cur_gecos[256], cur_dir[256], cur_shell[256];
    snprintf(cur_gecos, sizeof cur_gecos, "%s", pw->pw_gecos);
    snprintf(cur_dir,   sizeof cur_dir,   "%s", pw->pw_dir);
    snprintf(cur_shell, sizeof cur_shell, "%s", pw->pw_shell);
    uid_t u = pw->pw_uid;
    gid_t g = pw->pw_gid;

    if (group) {
        struct group *gr = getgrnam(group);
        if (!gr) die("unknown group %s", group);
        g = gr->gr_gid;
    }
    if (uid >= 0) u = (uid_t)uid;

    if (shell || home || group || uid >= 0) {
        char line[1024];
        snprintf(line, sizeof line, "%s:x:%u:%u:%s:%s:%s", name, (unsigned)u, (unsigned)g,
                 cur_gecos, home ? home : cur_dir, shell ? shell : cur_shell);
        lock_db();
        db_replace_line(PASSWD_FILE, 0644, name, line);
        unlock_db();
    }
    if (groups) {
        if (!append) {
            /* -G without -a replaces the membership list: drop the user from
             * every group that is not their primary one first. */
            char drop[64][64]; int nd = 0;
            setgrent();
            struct group *gr;
            while ((gr = getgrent()) && nd < 64) {
                if (gr->gr_gid == g) continue;
                for (char **m = gr->gr_mem; m && *m; m++)
                    if (!strcmp(*m, name)) { snprintf(drop[nd], sizeof drop[nd], "%s", gr->gr_name); nd++; break; }
            }
            endgrent();
            for (int i = 0; i < nd; i++) {
                struct group *x = getgrnam(drop[i]);
                if (!x) continue;
                char members[2048] = "";
                for (char **m = x->gr_mem; m && *m; m++) {
                    if (!strcmp(*m, name)) continue;
                    if (members[0]) strncat(members, ",", sizeof members - strlen(members) - 1);
                    strncat(members, *m, sizeof members - strlen(members) - 1);
                }
                char line[4096];
                snprintf(line, sizeof line, "%s:x:%u:%s", x->gr_name, (unsigned)x->gr_gid, members);
                lock_db(); db_replace_line(GROUP_FILE, 0644, x->gr_name, line); unlock_db();
            }
        }
        char buf[512]; snprintf(buf, sizeof buf, "%s", groups);
        char *save = NULL;
        for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            lock_db(); add_to_group(t, name); unlock_db();
        }
    }
    if (lock || unlock) {
        struct passwd *p2 = getpwnam(name);
        const char *cur = p2 ? stored_hash(p2) : NULL;
        char newhash[512];
        if (lock) snprintf(newhash, sizeof newhash, "!%s", cur && *cur && cur[0] != '!' ? cur : (cur ? cur + (cur[0] == '!') : ""));
        else snprintf(newhash, sizeof newhash, "%s", cur && cur[0] == '!' ? cur + 1 : (cur ? cur : ""));
        set_password(name, newhash);
    }
    return 0;
}

static int cmd_userdel(int argc, char **argv) {
    if (getuid() != 0) die("only root can remove users");
    const char *name = NULL;
    int remove_home = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) remove_home = 1;
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else name = argv[i];
    }
    if (!name) die("usage: userdel [-r] NAME");
    struct passwd *pw = getpwnam(name);
    if (!pw) die("unknown user %s", name);
    if (pw->pw_uid == 0) die("refusing to remove root");

    char home[256];
    snprintf(home, sizeof home, "%s", pw->pw_dir);
    struct group *gr = getgrnam(name);
    int own_group = gr && gr->gr_gid == pw->pw_gid && (!gr->gr_mem || !gr->gr_mem[0]);

    lock_db();
    db_replace_line(PASSWD_FILE, 0644, name, NULL);
    db_replace_line(SHADOW_FILE, 0640, name, NULL);
    if (own_group) db_replace_line(GROUP_FILE, 0644, name, NULL);
    unlock_db();

    if (remove_home && home[0] == '/' && strcmp(home, "/")) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "/usr/bin/rm -rf %s", home);
        if (system(cmd) != 0) fprintf(stderr, "%s: could not remove %s\n", prog, home);
    }
    return 0;
}

static int cmd_groupadd(int argc, char **argv) {
    if (getuid() != 0) die("only root can add groups");
    const char *name = NULL;
    long gid = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc) gid = atol(argv[++i]);
        else if (!strcmp(argv[i], "-r")) continue;
        else if (argv[i][0] == '-') die("unknown option %s", argv[i]);
        else name = argv[i];
    }
    if (!name) die("usage: groupadd [-g GID] NAME");
    if (getgrnam(name)) die("group %s already exists", name);
    lock_db();
    add_group(name, gid >= 0 ? (gid_t)gid : next_free_gid());
    unlock_db();
    return 0;
}

static int cmd_groupdel(int argc, char **argv) {
    if (getuid() != 0) die("only root can remove groups");
    if (argc < 2) die("usage: groupdel NAME");
    const char *name = argv[1];
    struct group *gr = getgrnam(name);
    if (!gr) die("unknown group %s", name);
    /* Refuse while it is somebody's primary group — otherwise their files end
     * up owned by a gid that resolves to nothing. */
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()))
        if (pw->pw_gid == gr->gr_gid) {
            endpwent();
            die("%s is the primary group of %s", name, pw->pw_name);
        }
    endpwent();
    lock_db();
    db_replace_line(GROUP_FILE, 0644, name, NULL);
    unlock_db();
    return 0;
}

/* nologin: the shell for accounts that must exist but must not be used. */
static int cmd_nologin(void) {
    FILE *f = fopen("/etc/nologin.txt", "r");
    if (f) {
        char buf[512];
        while (fgets(buf, sizeof buf, f)) fputs(buf, stdout);
        fclose(f);
    } else {
        printf("This account is not available.\n");
    }
    return 1;
}

static void usage(void) {
    printf("vlogin — VerkOS' account tools (call it by any of these names)\n\n"
           "  login [-f USER] [-h HOST]        authenticate and start a session\n"
           "  su [-|-l] [-c CMD] [USER]        become another user\n"
           "  passwd [-l|-u|-d] [USER]         change, lock, unlock or clear a password\n"
           "  chpasswd                         set passwords from USER:PASSWORD lines\n"
           "  useradd [-m] [-u UID] ... NAME   create an account\n"
           "  usermod [-aG GRPS] [-s SH] NAME  change an account\n"
           "  userdel [-r] NAME                remove an account\n"
           "  groupadd [-g GID] NAME           create a group\n"
           "  groupdel NAME                    remove a group\n"
           "  pwconv                           move passwords into /etc/shadow\n"
           "  nologin                          refuse a login\n");
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    if (*me == '-') me++;                    /* login shells get a '-' prefix */
    prog = me;

    if (!strcmp(me, "login"))    return cmd_login(argc, argv);
    if (!strcmp(me, "su"))       return cmd_su(argc, argv);
    if (!strcmp(me, "passwd"))   return cmd_passwd(argc, argv);
    if (!strcmp(me, "chpasswd")) return cmd_chpasswd(argc, argv);
    if (!strcmp(me, "useradd"))  return cmd_useradd(argc, argv);
    if (!strcmp(me, "usermod"))  return cmd_usermod(argc, argv);
    if (!strcmp(me, "userdel"))  return cmd_userdel(argc, argv);
    if (!strcmp(me, "groupadd")) return cmd_groupadd(argc, argv);
    if (!strcmp(me, "groupdel")) return cmd_groupdel(argc, argv);
    if (!strcmp(me, "pwconv"))   return cmd_pwconv(argc, argv);
    if (!strcmp(me, "nologin"))  return cmd_nologin();

    if (argc > 1) {
        char *sub = argv[1];
        argv[1] = argv[0];
        prog = sub;
        if (!strcmp(sub, "login"))    return cmd_login(argc - 1, argv + 1);
        if (!strcmp(sub, "su"))       return cmd_su(argc - 1, argv + 1);
        if (!strcmp(sub, "passwd"))   return cmd_passwd(argc - 1, argv + 1);
        if (!strcmp(sub, "chpasswd")) return cmd_chpasswd(argc - 1, argv + 1);
        if (!strcmp(sub, "useradd"))  return cmd_useradd(argc - 1, argv + 1);
        if (!strcmp(sub, "usermod"))  return cmd_usermod(argc - 1, argv + 1);
        if (!strcmp(sub, "userdel"))  return cmd_userdel(argc - 1, argv + 1);
        if (!strcmp(sub, "groupadd")) return cmd_groupadd(argc - 1, argv + 1);
        if (!strcmp(sub, "groupdel")) return cmd_groupdel(argc - 1, argv + 1);
        if (!strcmp(sub, "pwconv"))   return cmd_pwconv(argc - 1, argv + 1);
        if (!strcmp(sub, "nologin"))  return cmd_nologin();
    }
    usage();
    return 1;
}
