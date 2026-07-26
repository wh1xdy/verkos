/* Is this directory entry the name of a kernel?
 *
 * Kept in its own header so it can be tested on the build host: it is ordinary
 * C with no firmware in it, and the bug it exists to prevent is invisible until
 * a machine fails to boot. vboot used to compare "vmlinuz" byte for byte in
 * lower case, which finds nothing at all on a filesystem that hands back
 * upper-case names — an ISO 9660 volume without Rock Ridge gives VMLINUZ.;1.
 * The filesystem decides the case, not us.
 */
#ifndef VBOOT_KERNELNAME_H
#define VBOOT_KERNELNAME_H

static int name_is_kernel(const CHAR16 *nm) {
    static const CHAR16 want[7] = { 'v', 'm', 'l', 'i', 'n', 'u', 'z' };
    for (int i = 0; i < 7; i++) {
        CHAR16 c = nm[i];
        if (c >= 'A' && c <= 'Z') c = (CHAR16)(c + ('a' - 'A'));
        if (c != want[i]) return 0;
    }
    return 1;
}

#endif
