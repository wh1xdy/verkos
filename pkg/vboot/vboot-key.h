/* vboot-key.h — the public key vboot trusts.
 *
 * All zeroes means "no key configured": vboot then boots without checking, so an
 * unsigned build behaves exactly as it did before signing existed. To turn
 * verification on, generate a key pair and replace this file:
 *
 *     vsign keygen verkos.key verkos.pub
 *     vsign pubhdr verkos.pub > pkg/vboot/vboot-key.h
 *     vsign sign   verkos.key work/<arch>/out/vmlinuz     # and the initramfs
 *
 * Keep verkos.key off the machine you are securing — whoever holds it can sign
 * a kernel this bootloader will trust.
 */
static const unsigned char VBOOT_PUBKEY[32] = { 0 };
