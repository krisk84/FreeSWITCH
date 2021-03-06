/*
 * aes_icm_ossl.c
 *
 * AES Integer Counter Mode
 *
 * John A. Foley
 * Cisco Systems, Inc.
 *
 * 2/24/2012:  This module was modified to use CiscoSSL for AES counter
 *             mode.  Eddy Lem contributed the code to allow this.
 *
 * 12/20/2012: Added support for AES-192 and AES-256.
 */

/*
 *
 * Copyright (c) 2013, Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <openssl/evp.h>
#include "aes_icm_ossl.h"
#include "crypto_types.h"
#include "alloc.h"
#include "crypto_types.h"


debug_module_t mod_aes_icm = {
    0,               /* debugging is off by default */
    "aes icm ossl"   /* printable module name       */
};
extern cipher_test_case_t aes_icm_test_case_0;
extern cipher_type_t aes_icm;
extern cipher_type_t aes_icm_192;
extern cipher_type_t aes_icm_256;

/*
 * integer counter mode works as follows:
 *
 * 16 bits
 * <----->
 * +------+------+------+------+------+------+------+------+
 * |           nonce           |    pakcet index    |  ctr |---+
 * +------+------+------+------+------+------+------+------+   |
 *                                                             |
 * +------+------+------+------+------+------+------+------+   v
 * |                      salt                      |000000|->(+)
 * +------+------+------+------+------+------+------+------+   |
 *                                                             |
 *                                                        +---------+
 *							  | encrypt |
 *							  +---------+
 *							       |
 * +------+------+------+------+------+------+------+------+   |
 * |                    keystream block                    |<--+
 * +------+------+------+------+------+------+------+------+
 *
 * All fields are big-endian
 *
 * ctr is the block counter, which increments from zero for
 * each packet (16 bits wide)
 *
 * packet index is distinct for each packet (48 bits wide)
 *
 * nonce can be distinct across many uses of the same key, or
 * can be a fixed value per key, or can be per-packet randomness
 * (64 bits)
 *
 */

/*
 * This function allocates a new instance of this crypto engine.
 * The key_len parameter should be one of 30, 38, or 46 for
 * AES-128, AES-192, and AES-256 respectively.  Note, this key_len
 * value is inflated, as it also accounts for the 112 bit salt
 * value.
 */
err_status_t aes_icm_openssl_alloc (cipher_t **c, int key_len, int x)
{
    aes_icm_ctx_t *icm;
    int tmp;
    uint8_t *allptr;

    debug_print(mod_aes_icm, "allocating cipher with key length %d", key_len);

    /*
     * Verify the key_len is valid for one of: AES-128/192/256
     */
    if (key_len != AES_128_KEYSIZE_WSALT && key_len != AES_192_KEYSIZE_WSALT &&
        key_len != AES_256_KEYSIZE_WSALT) {
        return err_status_bad_param;
    }

    /* allocate memory a cipher of type aes_icm */
    tmp = sizeof(cipher_t) + sizeof(aes_icm_ctx_t);
    allptr = (uint8_t*)crypto_alloc(tmp);
    if (allptr == NULL) {
        return err_status_alloc_fail;
    }

    /* set pointers */
    *c = (cipher_t*)allptr;
    (*c)->state = allptr + sizeof(cipher_t);
    icm = (aes_icm_ctx_t*)(*c)->state;

    /* increment ref_count */
    switch (key_len) {
    case AES_128_KEYSIZE_WSALT:
        (*c)->algorithm = AES_128_ICM;
        (*c)->type = &aes_icm;
        aes_icm.ref_count++;
        ((aes_icm_ctx_t*)(*c)->state)->key_size = AES_128_KEYSIZE;
        break;
    case AES_192_KEYSIZE_WSALT:
        (*c)->algorithm = AES_192_ICM;
        (*c)->type = &aes_icm_192;
        aes_icm_192.ref_count++;
        ((aes_icm_ctx_t*)(*c)->state)->key_size = AES_192_KEYSIZE;
        break;
    case AES_256_KEYSIZE_WSALT:
        (*c)->algorithm = AES_256_ICM;
        (*c)->type = &aes_icm_256;
        aes_icm_256.ref_count++;
        ((aes_icm_ctx_t*)(*c)->state)->key_size = AES_256_KEYSIZE;
        break;
    }

    /* set key size        */
    (*c)->key_len = key_len;
    EVP_CIPHER_CTX_init(&icm->ctx);

    return err_status_ok;
}


/*
 * This function deallocates an instance of this engine
 */
err_status_t aes_icm_openssl_dealloc (cipher_t *c)
{
    aes_icm_ctx_t *ctx;

    if (c == NULL) {
        return err_status_bad_param;
    }

    /*
     * Free the EVP context
     */
    ctx = (aes_icm_ctx_t*)c->state;
    if (ctx != NULL) {
        EVP_CIPHER_CTX_cleanup(&ctx->ctx);
        /* decrement ref_count for the appropriate engine */
        switch (ctx->key_size) {
        case AES_256_KEYSIZE:
            aes_icm_256.ref_count--;
            break;
        case AES_192_KEYSIZE:
            aes_icm_192.ref_count--;
            break;
        case AES_128_KEYSIZE:
            aes_icm.ref_count--;
            break;
        default:
            return err_status_dealloc_fail;
            break;
        }
    }

    /* zeroize entire state*/
    octet_string_set_to_zero((uint8_t*)c,
                             sizeof(cipher_t) + sizeof(aes_icm_ctx_t));

    /* free memory */
    crypto_free(c);

    return err_status_ok;
}

/*
 * aes_icm_openssl_context_init(...) initializes the aes_icm_context
 * using the value in key[].
 *
 * the key is the secret key
 *
 * the salt is unpredictable (but not necessarily secret) data which
 * randomizes the starting point in the keystream
 */
err_status_t aes_icm_openssl_context_init (aes_icm_ctx_t *c, const uint8_t *key)
{
    /* set counter and initial values to 'offset' value */
    /* FIX!!! this assumes the salt is at key + 16, and thus that the */
    /* FIX!!! cipher key length is 16!  Also note this copies past the
              end of the 'key' array by 2 bytes! */
    v128_copy_octet_string(&c->counter, key + c->key_size);
    v128_copy_octet_string(&c->offset, key + c->key_size);

    /* force last two octets of the offset to zero (for srtp compatibility) */
    c->offset.v8[SALT_SIZE] = c->offset.v8[SALT_SIZE + 1] = 0;
    c->counter.v8[SALT_SIZE] = c->counter.v8[SALT_SIZE + 1] = 0;

    /* copy key to be used later when CiscoSSL crypto context is created */
    v128_copy_octet_string((v128_t*)&c->key, key);

    /* if the key is greater than 16 bytes, copy the second
     * half.  Note, we treat AES-192 and AES-256 the same here
     * for simplicity.  The storage location receiving the
     * key is statically allocated to handle a full 32 byte key
     * regardless of the cipher in use.
     */
    if (c->key_size == AES_256_KEYSIZE || c->key_size == AES_192_KEYSIZE) {
        debug_print(mod_aes_icm, "Copying last 16 bytes of key: %s",
                    v128_hex_string((v128_t*)(key + AES_128_KEYSIZE)));
        v128_copy_octet_string(((v128_t*)(&c->key.v8)) + 1, key + AES_128_KEYSIZE);
    }

    debug_print(mod_aes_icm, "key:  %s", v128_hex_string((v128_t*)&c->key));
    debug_print(mod_aes_icm, "offset: %s", v128_hex_string(&c->offset));

    EVP_CIPHER_CTX_cleanup(&c->ctx);

    return err_status_ok;
}


/*
 * aes_icm_set_iv(c, iv) sets the counter value to the exor of iv with
 * the offset
 */
err_status_t aes_icm_openssl_set_iv (aes_icm_ctx_t *c, void *iv, int dir)
{
    const EVP_CIPHER *evp;
    v128_t *nonce = (v128_t*)iv;

    debug_print(mod_aes_icm, "setting iv: %s", v128_hex_string(nonce));

    v128_xor(&c->counter, &c->offset, nonce);

    debug_print(mod_aes_icm, "set_counter: %s", v128_hex_string(&c->counter));

    switch (c->key_size) {
    case AES_256_KEYSIZE:
        evp = EVP_aes_256_ctr();
        break;
    case AES_192_KEYSIZE:
        evp = EVP_aes_192_ctr();
        break;
    case AES_128_KEYSIZE:
        evp = EVP_aes_128_ctr();
        break;
    default:
        return err_status_bad_param;
        break;
    }

    if (!EVP_EncryptInit_ex(&c->ctx, evp,
                            NULL, c->key.v8, c->counter.v8)) {
        return err_status_fail;
    } else {
        return err_status_ok;
    }
}

/*
 * This function encrypts a buffer using AES CTR mode
 *
 * Parameters:
 *	c	Crypto context
 *	buf	data to encrypt
 *	enc_len	length of encrypt buffer
 */
err_status_t aes_icm_openssl_encrypt (aes_icm_ctx_t *c, unsigned char *buf, unsigned int *enc_len)
{
    int len = 0;

    debug_print(mod_aes_icm, "rs0: %s", v128_hex_string(&c->counter));

    if (!EVP_EncryptUpdate(&c->ctx, buf, &len, buf, *enc_len)) {
        return err_status_cipher_fail;
    }
    *enc_len = len;

    if (!EVP_EncryptFinal_ex(&c->ctx, buf, (int*)&len)) {
        return err_status_cipher_fail;
    }
    *enc_len += len;

    return err_status_ok;
}

/*
 * Abstraction layer for encrypt.
 */
err_status_t aes_icm_output (aes_icm_ctx_t *c, uint8_t *buffer, int num_octets_to_output)
{
    unsigned int len = num_octets_to_output;

    /* zeroize the buffer */
    octet_string_set_to_zero(buffer, num_octets_to_output);

    /* exor keystream into buffer */
    return aes_icm_openssl_encrypt(c, buffer, &len);
}

/*
 * Name of this crypto engine
 */
char aes_icm_openssl_description[] = "AES-128 counter mode using openssl";
char aes_icm_192_openssl_description[] = "AES-192 counter mode using openssl";
char aes_icm_256_openssl_description[] = "AES-256 counter mode using openssl";


/*
 * KAT values for AES self-test.  These
 * values came from the legacy libsrtp code.
 */
uint8_t aes_icm_test_case_0_key[AES_128_KEYSIZE_WSALT] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd
};

uint8_t aes_icm_test_case_0_nonce[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t aes_icm_test_case_0_plaintext[32] =  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint8_t aes_icm_test_case_0_ciphertext[32] = {
    0xe0, 0x3e, 0xad, 0x09, 0x35, 0xc9, 0x5e, 0x80,
    0xe1, 0x66, 0xb1, 0x6d, 0xd9, 0x2b, 0x4e, 0xb4,
    0xd2, 0x35, 0x13, 0x16, 0x2b, 0x02, 0xd0, 0xf7,
    0x2a, 0x43, 0xa2, 0xfe, 0x4a, 0x5f, 0x97, 0xab
};

cipher_test_case_t aes_icm_test_case_0 = {
    AES_128_KEYSIZE_WSALT,                 /* octets in key            */
    aes_icm_test_case_0_key,               /* key                      */
    aes_icm_test_case_0_nonce,             /* packet index             */
    32,                                    /* octets in plaintext      */
    aes_icm_test_case_0_plaintext,         /* plaintext                */
    32,                                    /* octets in ciphertext     */
    aes_icm_test_case_0_ciphertext,        /* ciphertext               */
    0,
    NULL,
    NULL                                   /* pointer to next testcase */
};

/*
 * KAT values for AES-192-CTR self-test.  These
 * values came from section 7 of RFC 6188.
 */
uint8_t aes_icm_192_test_case_1_key[AES_192_KEYSIZE_WSALT] = {
    0xea, 0xb2, 0x34, 0x76, 0x4e, 0x51, 0x7b, 0x2d,
    0x3d, 0x16, 0x0d, 0x58, 0x7d, 0x8c, 0x86, 0x21,
    0x97, 0x40, 0xf6, 0x5f, 0x99, 0xb6, 0xbc, 0xf7,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd
};

uint8_t aes_icm_192_test_case_1_nonce[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t aes_icm_192_test_case_1_plaintext[32] =  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint8_t aes_icm_192_test_case_1_ciphertext[32] = {
    0x35, 0x09, 0x6c, 0xba, 0x46, 0x10, 0x02, 0x8d,
    0xc1, 0xb5, 0x75, 0x03, 0x80, 0x4c, 0xe3, 0x7c,
    0x5d, 0xe9, 0x86, 0x29, 0x1d, 0xcc, 0xe1, 0x61,
    0xd5, 0x16, 0x5e, 0xc4, 0x56, 0x8f, 0x5c, 0x9a
};

cipher_test_case_t aes_icm_192_test_case_1 = {
    AES_192_KEYSIZE_WSALT,                 /* octets in key            */
    aes_icm_192_test_case_1_key,           /* key                      */
    aes_icm_192_test_case_1_nonce,         /* packet index             */
    32,                                    /* octets in plaintext      */
    aes_icm_192_test_case_1_plaintext,     /* plaintext                */
    32,                                    /* octets in ciphertext     */
    aes_icm_192_test_case_1_ciphertext,    /* ciphertext               */
    0,
    NULL,
    NULL                                   /* pointer to next testcase */
};


/*
 * KAT values for AES-256-CTR self-test.  These
 * values came from section 7 of RFC 6188.
 */
uint8_t aes_icm_256_test_case_2_key[AES_256_KEYSIZE_WSALT] = {
    0x57, 0xf8, 0x2f, 0xe3, 0x61, 0x3f, 0xd1, 0x70,
    0xa8, 0x5e, 0xc9, 0x3c, 0x40, 0xb1, 0xf0, 0x92,
    0x2e, 0xc4, 0xcb, 0x0d, 0xc0, 0x25, 0xb5, 0x82,
    0x72, 0x14, 0x7c, 0xc4, 0x38, 0x94, 0x4a, 0x98,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd
};

uint8_t aes_icm_256_test_case_2_nonce[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t aes_icm_256_test_case_2_plaintext[32] =  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint8_t aes_icm_256_test_case_2_ciphertext[32] = {
    0x92, 0xbd, 0xd2, 0x8a, 0x93, 0xc3, 0xf5, 0x25,
    0x11, 0xc6, 0x77, 0xd0, 0x8b, 0x55, 0x15, 0xa4,
    0x9d, 0xa7, 0x1b, 0x23, 0x78, 0xa8, 0x54, 0xf6,
    0x70, 0x50, 0x75, 0x6d, 0xed, 0x16, 0x5b, 0xac
};

cipher_test_case_t aes_icm_256_test_case_2 = {
    AES_256_KEYSIZE_WSALT,                 /* octets in key            */
    aes_icm_256_test_case_2_key,           /* key                      */
    aes_icm_256_test_case_2_nonce,         /* packet index             */
    32,                                    /* octets in plaintext      */
    aes_icm_256_test_case_2_plaintext,     /* plaintext                */
    32,                                    /* octets in ciphertext     */
    aes_icm_256_test_case_2_ciphertext,    /* ciphertext               */
    0,
    NULL,
    NULL                                   /* pointer to next testcase */
};

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
cipher_type_t aes_icm = {
    (cipher_alloc_func_t)          aes_icm_openssl_alloc,
    (cipher_dealloc_func_t)        aes_icm_openssl_dealloc,
    (cipher_init_func_t)           aes_icm_openssl_context_init,
    (cipher_set_aad_func_t)        0,
    (cipher_encrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_decrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_set_iv_func_t)         aes_icm_openssl_set_iv,
    (cipher_get_tag_func_t)        0,
    (char*)                        aes_icm_openssl_description,
    (int)                          0,            /* instance count */
    (cipher_test_case_t*)          &aes_icm_test_case_0,
    (debug_module_t*)              &mod_aes_icm,
    (cipher_type_id_t)             AES_ICM
};

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
cipher_type_t aes_icm_192 = {
    (cipher_alloc_func_t)          aes_icm_openssl_alloc,
    (cipher_dealloc_func_t)        aes_icm_openssl_dealloc,
    (cipher_init_func_t)           aes_icm_openssl_context_init,
    (cipher_set_aad_func_t)        0,
    (cipher_encrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_decrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_set_iv_func_t)         aes_icm_openssl_set_iv,
    (cipher_get_tag_func_t)        0,
    (char*)                        aes_icm_192_openssl_description,
    (int)                          0,                /* instance count */
    (cipher_test_case_t*)          &aes_icm_192_test_case_1,
    (debug_module_t*)              &mod_aes_icm,
    (cipher_type_id_t)             AES_192_ICM
};

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
cipher_type_t aes_icm_256 = {
    (cipher_alloc_func_t)          aes_icm_openssl_alloc,
    (cipher_dealloc_func_t)        aes_icm_openssl_dealloc,
    (cipher_init_func_t)           aes_icm_openssl_context_init,
    (cipher_set_aad_func_t)        0,
    (cipher_encrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_decrypt_func_t)        aes_icm_openssl_encrypt,
    (cipher_set_iv_func_t)         aes_icm_openssl_set_iv,
    (cipher_get_tag_func_t)        0,
    (char*)                        aes_icm_256_openssl_description,
    (int)                          0,                /* instance count */
    (cipher_test_case_t*)          &aes_icm_256_test_case_2,
    (debug_module_t*)              &mod_aes_icm,
    (cipher_type_id_t)             AES_256_ICM
};

