/* passwd — change a password. Setuid root, and therefore its own binary.
 *
 * As with su: one behaviour, fixed at compile time, unreachable from any
 * other. See D-051 for why that matters more than the duplication costs.
 */
#include "vlogin_core.h"

int main(int argc, char **argv) {
    vlogin_secure_startup();
    prog = "passwd";
    return cmd_passwd(argc, argv);
}
