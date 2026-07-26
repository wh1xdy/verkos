/* su — switch user. Setuid root, and therefore its own binary.
 *
 * It cannot be told to be anything else. There is no dispatch here, nothing
 * reads argv[0], and no name a caller supplies changes what runs. That is the
 * whole point of the file existing: the multi-call binary chooses its
 * behaviour from argv[0], and argv[0] is chosen by whoever calls execve.
 */
#include "vlogin_core.h"

int main(int argc, char **argv) {
    vlogin_secure_startup();
    prog = "su";
    return cmd_su(argc, argv);
}
