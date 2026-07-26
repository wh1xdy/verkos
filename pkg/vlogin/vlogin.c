/* vlogin — the account tools as one multi-call binary.
 *
 * This one dispatches on argv[0], the way busybox and every multi-call tool
 * does, and that is safe here for exactly one reason: **this binary is not
 * setuid**. Whoever runs it gets what they already had. Choosing behaviour
 * from a string the caller controls is only dangerous when the binary carries
 * privileges the caller does not.
 *
 * su and passwd are not in this list. They are separate binaries, because they
 * are the two that must run as root. See D-051.
 */
#include <string.h>
#include <stdio.h>
#include "vlogin_core.h"

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    if (*me == '-') me++;                    /* login shells get a '-' prefix */
    prog = me;

    if (!strcmp(me, "login"))    return cmd_login(argc, argv);
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
        if (!strcmp(sub, "chpasswd")) return cmd_chpasswd(argc - 1, argv + 1);
        if (!strcmp(sub, "useradd"))  return cmd_useradd(argc - 1, argv + 1);
        if (!strcmp(sub, "usermod"))  return cmd_usermod(argc - 1, argv + 1);
        if (!strcmp(sub, "userdel"))  return cmd_userdel(argc - 1, argv + 1);
        if (!strcmp(sub, "groupadd")) return cmd_groupadd(argc - 1, argv + 1);
        if (!strcmp(sub, "groupdel")) return cmd_groupdel(argc - 1, argv + 1);
        if (!strcmp(sub, "pwconv"))   return cmd_pwconv(argc - 1, argv + 1);
        if (!strcmp(sub, "nologin"))  return cmd_nologin();
    }

    fprintf(stderr,
        "vlogin — VerkOS account tools\n\n"
        "  login | chpasswd | useradd | usermod | userdel\n"
        "  groupadd | groupdel | pwconv | nologin\n\n"
        "su and passwd are separate programs: they run as root, and a binary\n"
        "that runs as root must not be able to become a different tool.\n");
    return 2;
}
