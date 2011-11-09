#include "system.h"

#include <rpm/rpmstring.h>
#include <rpm/rpmpgp.h>
#include <rpm/rpmfileutil.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmkeyring.h>

#include "rpmio/base64.h"
#include "rpmio/digest.h"

#include "debug.h"

struct rpmPubkey_s {
    uint8_t *pkt;
    size_t pktlen;
    pgpKeyID_t keyid;
    pgpDigParams pgpkey;
    int nrefs;
};

struct rpmKeyring_s {
    struct rpmPubkey_s **keys;
    size_t numkeys;
    int nrefs;
};

static rpmPubkey rpmPubkeyUnlink(rpmPubkey key);
static rpmKeyring rpmKeyringUnlink(rpmKeyring keyring);

static int keyidcmp(const void *k1, const void *k2)
{
    const struct rpmPubkey_s *key1 = *(const struct rpmPubkey_s **) k1;
    const struct rpmPubkey_s *key2 = *(const struct rpmPubkey_s **) k2;
    
    return memcmp(key1->keyid, key2->keyid, sizeof(key1->keyid));
}

rpmKeyring rpmKeyringNew(void)
{
    rpmKeyring keyring = xcalloc(1, sizeof(*keyring));
    keyring->keys = NULL;
    keyring->numkeys = 0;
    keyring->nrefs = 0;
    return rpmKeyringLink(keyring);
}

rpmKeyring rpmKeyringFree(rpmKeyring keyring)
{
    if (keyring == NULL) {
	return NULL;
    }

    if (keyring->nrefs > 1) {
	return rpmKeyringUnlink(keyring);
    }

    if (keyring->keys) {
	for (int i = 0; i < keyring->numkeys; i++) {
	    keyring->keys[i] = rpmPubkeyFree(keyring->keys[i]);
	}
	free(keyring->keys);
    }
    free(keyring);
    return NULL;
}

static rpmPubkey rpmKeyringFindKeyid(rpmKeyring keyring, rpmPubkey key)
{
    rpmPubkey *found = NULL;
    found = bsearch(&key, keyring->keys, keyring->numkeys, sizeof(*keyring->keys), keyidcmp);
    return found ? *found : NULL;
}

int rpmKeyringAddKey(rpmKeyring keyring, rpmPubkey key)
{
    if (keyring == NULL || key == NULL)
	return -1;

    /* check if we already have this key */
    if (rpmKeyringFindKeyid(keyring, key)) {
	return 1;
    }
    
    keyring->keys = xrealloc(keyring->keys, (keyring->numkeys + 1) * sizeof(rpmPubkey));
    keyring->keys[keyring->numkeys] = rpmPubkeyLink(key);
    keyring->numkeys++;
    qsort(keyring->keys, keyring->numkeys, sizeof(*keyring->keys), keyidcmp);

    return 0;
}

rpmKeyring rpmKeyringLink(rpmKeyring keyring)
{
    if (keyring) {
	keyring->nrefs++;
    }
    return keyring;
}

static rpmKeyring rpmKeyringUnlink(rpmKeyring keyring)
{
    if (keyring) {
	keyring->nrefs--;
    }
    return NULL;
}

rpmPubkey rpmPubkeyRead(const char *filename)
{
    uint8_t *pkt = NULL;
    size_t pktlen;
    rpmPubkey key = NULL;

    if (pgpReadPkts(filename, &pkt, &pktlen) <= 0) {
	goto exit;
    }
    key = rpmPubkeyNew(pkt, pktlen);
    free(pkt);

exit:
    return key;
}

rpmPubkey rpmPubkeyNew(const uint8_t *pkt, size_t pktlen)
{
    rpmPubkey key = NULL;
    pgpDigParams pgpkey = NULL;
    pgpKeyID_t keyid;
    
    if (pkt == NULL || pktlen == 0)
	goto exit;

    if (pgpPubkeyFingerprint(pkt, pktlen, keyid))
	goto exit;

    if (pgpPrtParams(pkt, pktlen, PGPTAG_PUBLIC_KEY, &pgpkey))
	goto exit;

    key = xcalloc(1, sizeof(*key));
    key->pkt = xmalloc(pktlen);
    key->pktlen = pktlen;
    key->pgpkey = pgpkey;
    key->nrefs = 0;
    memcpy(key->pkt, pkt, pktlen);
    memcpy(key->keyid, keyid, sizeof(keyid));

exit:
    return rpmPubkeyLink(key);
}

rpmPubkey rpmPubkeyFree(rpmPubkey key)
{
    if (key == NULL)
	return NULL;

    if (key->nrefs > 1)
	return rpmPubkeyUnlink(key);

    pgpDigParamsFree(key->pgpkey);
    free(key->pkt);
    free(key);
    return NULL;
}

rpmPubkey rpmPubkeyLink(rpmPubkey key)
{
    if (key) {
	key->nrefs++;
    }
    return key;
}

static rpmPubkey rpmPubkeyUnlink(rpmPubkey key)
{
    if (key) {
	key->nrefs--;
    }
    return NULL;
}

pgpDig rpmPubkeyDig(rpmPubkey key)
{
    pgpDig dig = NULL;
    static unsigned char zeros[] = 
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int rc;
    if (key == NULL)
	return NULL;

    dig = pgpNewDig();
    rc = pgpPrtPkts(key->pkt, key->pktlen, dig, 0);
    if (rc == 0) {
	pgpDigParams pubp = pgpDigGetParams(dig, PGPTAG_PUBLIC_KEY);
	if (!pubp || !memcmp(pubp->signid, zeros, sizeof(pubp->signid)) ||
		!memcmp(pubp->time, zeros, sizeof(pubp->time)) ||
		pubp->userid == NULL) {
	    rc = -1;
	}
    }

    if (rc)
	dig = pgpFreeDig(dig);

    return dig;
}

char * rpmPubkeyBase64(rpmPubkey key)
{
    char *enc = NULL;

    if (key) {
	enc = b64encode(key->pkt, key->pktlen, -1);
    }
    return enc;
}

rpmRC rpmKeyringLookup(rpmKeyring keyring, pgpDig sig)
{
    rpmRC res = RPMRC_NOKEY;
    pgpDigParams sigp = pgpDigGetParams(sig, PGPTAG_SIGNATURE);

    if (keyring && sigp) {
	struct rpmPubkey_s needle, *key;
	needle.pkt = NULL;
	needle.pktlen = 0;
	memcpy(needle.keyid, sigp->signid, sizeof(needle.keyid));

	if ((key = rpmKeyringFindKeyid(keyring, &needle))) {
	    /* Retrieve parameters from pubkey packet(s) */
	    int pktrc = pgpPrtPkts(key->pkt, key->pktlen, sig, 0);
	    pgpDigParams pubp = pgpDigGetParams(sig, PGPTAG_PUBLIC_KEY);
	    /* Do the parameters match the signature? */
	    if (pubp && pktrc == 0 && sigp->pubkey_algo == pubp->pubkey_algo &&
		memcmp(sigp->signid, pubp->signid, sizeof(sigp->signid)) == 0) {
		res = RPMRC_OK;
	    }
	}
    }

    return res;
}
