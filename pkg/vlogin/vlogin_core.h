/* vlogin_core.h — the account tools, as a library.
 *
 * The split exists for one reason: `su` and `passwd` run setuid root, and a
 * setuid binary must not be able to become a different tool. See D-051.
 *
 * Everything here is shared by three programs:
 *   vlogin   the multi-call tool, NOT setuid, dispatches on argv[0]
 *   su       one main(), one behaviour, setuid root
 *   passwd   one main(), one behaviour, setuid root
 */
#ifndef VLOGIN_CORE_H
#define VLOGIN_CORE_H

/* The name used in error messages. The multi-call binary sets it from argv[0];
 * the setuid binaries set it to a constant, because their own name is not
 * something a caller gets to choose. */
extern const char *prog;

/* Called first by any binary that runs with privileges its caller lacks. */
void vlogin_secure_startup(void);

int cmd_login(int argc, char **argv);
int cmd_su(int argc, char **argv);
int cmd_passwd(int argc, char **argv);
int cmd_chpasswd(int argc, char **argv);
int cmd_pwconv(int argc, char **argv);
int cmd_useradd(int argc, char **argv);
int cmd_usermod(int argc, char **argv);
int cmd_userdel(int argc, char **argv);
int cmd_groupadd(int argc, char **argv);
int cmd_groupdel(int argc, char **argv);
int cmd_nologin(void);

#endif
