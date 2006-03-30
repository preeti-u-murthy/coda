/* BLURB lgpl
			Coda File System
			    Release 6

	    Copyright (c) 2006 Carnegie Mellon University
		  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the  terms of the  GNU  Library General Public Licence  Version 2,  as
shown in the file LICENSE. The technical and financial contributors to
Coda are listed in the file CREDITS.

			Additional copyrights
#*/

#include <rpc2/secure.h>
#include "aes.h"
#include "grunt.h"

/* Authentication algorithms. */
extern struct secure_auth
    secure_AUTH_NONE,
    secure_AUTH_AES_XCBC_MAC_96;

static const struct secure_auth *alg_auth[] = {
    &secure_AUTH_NONE,
    &secure_AUTH_AES_XCBC_MAC_96,
    NULL
};

/* Encryption algorithms. */
extern struct secure_encr
    secure_ENCR_NULL,
    secure_ENCR_AES_CBC,
    secure_ENCR_AES_CCM_8,
    secure_ENCR_AES_CCM_12,
    secure_ENCR_AES_CCM_16;

static const struct secure_encr *alg_encr[] = {
    &secure_ENCR_NULL,
    &secure_ENCR_AES_CBC,
    &secure_ENCR_AES_CCM_8,
    &secure_ENCR_AES_CCM_12,
    &secure_ENCR_AES_CCM_16,
    NULL
};

void secure_init(int verbose)
{
    /* Initialize and run the AES test vectors */
    secure_aes_init(verbose);

    /* Initialize and test the PRNG */
    secure_random_init(verbose);
}

void secure_release(void)
{
    secure_random_release();
}

const struct secure_auth *secure_get_auth_byid(int id)
{
    int i = 0;
    while (alg_auth[i] && id != alg_auth[i]->id) i++;
    return alg_auth[i];
}

const struct secure_encr *secure_get_encr_byid(int id)
{
    int i = 0;
    while (alg_encr[i] && alg_encr[i]->id != id) i++;
    return alg_encr[i];
}

int secure_setup_encrypt(struct security_association *sa,
			 const struct secure_auth *authenticate,
			 const struct secure_encr *encrypt,
			 const uint8_t *key, size_t len)
{
    int rc, min_keysize = encrypt ? encrypt->min_keysize : 0;

    /* clear any existing decryption/validation state */
    if (sa->authenticate) {
	sa->authenticate->auth_free(&sa->authenticate_context);
	sa->authenticate = NULL;
    }

    if (sa->encrypt) {
	sa->encrypt->encrypt_free(&sa->encrypt_context);
	sa->encrypt = NULL;
    }

    /* intialize new state */
    if (authenticate) {
	rc = authenticate->auth_init(&sa->authenticate_context, key, len);
	if (rc) return -1;
	sa->authenticate = authenticate;

	/* if we have enough key material, keep authentication and decryption
	 * keys separate, otherwise we just have to reuse the same key data */
	if (len >= authenticate->keysize + min_keysize)
	{
	    key += authenticate->keysize;
	    len -= authenticate->keysize;
	}
    }

    if (encrypt) {
	rc = encrypt->encrypt_init(&sa->encrypt_context, key, len);
	if (rc) return -1;
	sa->encrypt = encrypt;
    }
    return 0;
}

int secure_setup_decrypt(struct security_association *sa,
			 const struct secure_auth *validate,
			 const struct secure_encr *decrypt,
			 const uint8_t *key, size_t len)
{
    int rc, min_keysize = decrypt ? decrypt->min_keysize : 0;

    /* clear any existing decryption/validation state */
    if (sa->validate) {
	sa->validate->auth_free(&sa->validate_context);
	sa->validate = NULL;
    }

    if (sa->decrypt) {
	sa->decrypt->decrypt_free(&sa->decrypt_context);
	sa->decrypt = NULL;
    }

    /* intialize new state */
    if (validate) {
	rc = validate->auth_init(&sa->validate_context, key, len);
	if (rc) return -1;
	sa->validate = validate;

	/* if we have enough key material, keep authentication and decryption
	 * keys separate, otherwise we just have to reuse the same key data */
	if (len >= validate->keysize + min_keysize)
	{
	    key += validate->keysize;
	    len -= validate->keysize;
	}
    }

    if (decrypt) {
	rc = decrypt->decrypt_init(&sa->decrypt_context, key, len);
	if (rc) return -1;
	sa->decrypt = decrypt;
    }

    secure_random_bytes(&sa->send_iv, sizeof(sa->send_iv));
    return 0;
}

