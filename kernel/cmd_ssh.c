#include <stdint.h>
#include "commands.h"
#include "ssh.h"
#include "users.h"
#include "fs.h"
#include "crypto.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_ssh.c
 *  `sshd` (run the server) and `sshkey` (show the host key fingerprint).
 *
 *  The server runs in the foreground and stops on a keypress, like `serve`.
 *  This kernel has no daemons, and for a machine you are sitting in front of
 *  while setting it up, a server you can watch running and stop with a key is
 *  more honest than one that disappears into the background with no way to
 *  tell whether it is listening.
 * ===========================================================================*/

int cmd_sshd(int argc, char **argv) {
    uint16_t port = SSH_PORT;
    if (argc > 1) {
        if (!strcmp(argv[1], "key")) return cmd_sshkey(argc - 1, argv + 1);
        int v = atoi(argv[1]);
        if (v <= 0 || v > 65535) { kprintf("sshd: bad port '%s'\n", argv[1]); return 1; }
        port = (uint16_t)v;
    }

    /* Port 22 is privileged like every other well-known port: an account that
     * could bind it would be able to impersonate the ssh server to anyone who
     * connected, and collect their password while doing it. */
    if (port < 1024 && !users_is_root()) {
        kprintf("sshd: port %u is privileged -- run it under sudo\n", port);
        return 1;
    }
    return ssh_server_run(port) == 0 ? 0 : 1;
}

/* `sshkey selftest` signs and verifies with a throwaway key. It exists because
 * a wrong signature and a wrong exchange hash look identical from the client's
 * side -- both come back as "incorrect signature" -- and this tells them apart
 * without needing a client at all. */
static int selftest(void) {
    uint8_t priv[32], pub[65], nonce[32], h[32], r[32], sg[32];
    crypto_rand_bytes(priv, 32);
    p256_clamp_priv(priv);
    p256_pub_from_priv(priv, pub);
    for (int i = 0; i < 32; i++) h[i] = (uint8_t)(i * 7 + 1);

    int pass = 0, fail = 0, refused = 0;
    for (int t = 0; t < 8; t++) {
        crypto_rand_bytes(nonce, 32);
        if (p256_ecdsa_sign(h, 32, priv, nonce, r, sg) != 0) { refused++; continue; }
        if (p256_ecdsa_verify(h, 32, pub, r, sg) == 0) pass++;
        else fail++;
    }
    kprintf("ecdsa p256 sign/verify: %d ok, %d bad, %d nonces refused\n",
            pass, fail, refused);
    return (fail || !pass) ? 1 : 0;
}

int cmd_sshkey(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "selftest")) return selftest();

    if (ssh_hostkey_load() != 0) { kprintf("sshkey: no host key\n"); return 1; }
    char fp[80];
    ssh_hostkey_fingerprint(fp, sizeof fp);
    kprintf("host key: ecdsa-sha2-nistp256\n");
    kprintf("stored  : %s\n", SSH_HOSTKEY_PATH);
    kprintf("finger  : %s\n", fp);
    kprintf("\nThis is the fingerprint an ssh client shows on a first connection.\n"
            "If it does not match what you see there, do not type your password.\n");
    return 0;
}
