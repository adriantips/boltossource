#pragma once
#include <stdint.h>

/* ===========================================================================
 *  SSH-2 server (RFC 4251-4254) and a matching client.
 *
 *  This is what makes the machine administrable without sitting in front of
 *  it: a shell over the network, authenticated against the real account table,
 *  with the session encrypted. For a machine meant to be developed on, that is
 *  the difference between a demo and a box you can actually work on.
 *
 *  One algorithm per slot, all of them ones the crypto already had:
 *
 *      key exchange   curve25519-sha256        (X25519 + SHA-256)
 *      host key       ecdsa-sha2-nistp256      (P-256, signing added for this)
 *      cipher         aes128-ctr               (AES-128 in counter mode)
 *      MAC            hmac-sha2-256
 *      compression    none
 *
 *  Offering exactly one of each is deliberate. Every extra algorithm is more
 *  negotiation surface facing an unauthenticated peer, and this set is still
 *  in the default proposal of a current OpenSSH, so real clients connect
 *  without being told to lower anything.
 *
 *  The host key is generated once, on first use, and persisted to
 *  /etc/ssh/host_ecdsa_key with mode 0600 -- a host key that changed every boot
 *  would train the user to click through the warning that exists to tell them
 *  they are being intercepted.
 * ===========================================================================*/

#define SSH_PORT          22
#define SSH_HOSTKEY_PATH  "/etc/ssh/host_ecdsa_key"

/* Load the host key, generating and saving one if there is none yet.
 * Returns 0 on success. */
int  ssh_hostkey_load(void);

/* The SHA-256 fingerprint of the public host key, formatted the way OpenSSH
 * prints it ("SHA256:base64"), so a user can compare the two by eye.
 * `out` needs 64 bytes. */
void ssh_hostkey_fingerprint(char *out, uint32_t cap);

/* Run the server: bind `port`, then serve connections until a key is pressed.
 * Blocking, like `serve` -- this kernel has no daemons, and a server you can
 * see and stop is a better fit for it than one you cannot. */
int  ssh_server_run(uint16_t port);
