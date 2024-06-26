/*
 * Copyright 2023 The Tongsuo Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://github.com/Tongsuo-Project/Tongsuo/blob/master/LICENSE.txt
 */

#include <openssl/err.h>
#include <crypto/ec.h>
#include <crypto/ec/ec_local.h>
#include <crypto/zkp/common/zkp_util.h>
#include "nizk_dlog_knowledge.h"

NIZK_DLOG_KNOWLEDGE_CTX *NIZK_DLOG_KNOWLEDGE_CTX_new(ZKP_TRANSCRIPT *transcript,
                                                     NIZK_PUB_PARAM *pp,
                                                     NIZK_WITNESS *witness)
{
    NIZK_DLOG_KNOWLEDGE_CTX *ctx = NULL;

    if (pp == NULL || transcript == NULL) {
        ERR_raise(ERR_LIB_ZKP_NIZK, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }

    ctx = OPENSSL_zalloc(sizeof(*ctx));
    if (ctx == NULL) {
        ERR_raise(ERR_LIB_ZKP_NIZK, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    ctx->transcript = transcript;

    if (!NIZK_PUB_PARAM_up_ref(pp))
        goto err;

    ctx->pp = pp;

    if (witness != NULL) {
        if (!NIZK_WITNESS_up_ref(witness))
            goto err;

        ctx->witness = witness;
    }

    return ctx;

err:
    NIZK_DLOG_KNOWLEDGE_CTX_free(ctx);
    return NULL;
}

void NIZK_DLOG_KNOWLEDGE_CTX_free(NIZK_DLOG_KNOWLEDGE_CTX *ctx)
{
    if (ctx == NULL)
        return;

    NIZK_PUB_PARAM_down_ref(ctx->pp);
    NIZK_WITNESS_down_ref(ctx->witness);

    OPENSSL_clear_free((void *)ctx, sizeof(*ctx));
}

NIZK_DLOG_KNOWLEDGE_PROOF *NIZK_DLOG_KNOWLEDGE_PROOF_new(NIZK_DLOG_KNOWLEDGE_CTX *ctx)
{
    NIZK_DLOG_KNOWLEDGE_PROOF *proof = NULL;

    proof = OPENSSL_zalloc(sizeof(*proof));
    if (proof == NULL) {
        ERR_raise(ERR_LIB_ZKP_NIZK, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    if ((proof->A = EC_POINT_new(ctx->pp->group)) == NULL
        || (proof->z = BN_new()) == NULL)
        goto err;

    EC_POINT_set_to_infinity(ctx->pp->group, proof->A);

    BN_zero(proof->z);

    return proof;
err:
    NIZK_DLOG_KNOWLEDGE_PROOF_free(proof);
    return NULL;
}

void NIZK_DLOG_KNOWLEDGE_PROOF_free(NIZK_DLOG_KNOWLEDGE_PROOF *proof)
{
    if (proof == NULL)
        return;

    EC_POINT_free(proof->A);
    BN_free(proof->z);
    OPENSSL_clear_free((void *)proof, sizeof(*proof));
}

NIZK_DLOG_KNOWLEDGE_PROOF *NIZK_DLOG_KNOWLEDGE_PROOF_prove(NIZK_DLOG_KNOWLEDGE_CTX *ctx)
{
    ZKP_TRANSCRIPT *transcript;
    NIZK_PUB_PARAM *pp;
    NIZK_WITNESS *witness;
    NIZK_DLOG_KNOWLEDGE_PROOF *proof = NULL, *ret = NULL;
    const BIGNUM *order;
    EC_GROUP *group;
    BN_CTX *bn_ctx = NULL;
    BIGNUM *a, *e, *t;

    if (ctx == NULL) {
        ERR_raise(ERR_LIB_ZKP_NIZK, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }

    if (!(proof = NIZK_DLOG_KNOWLEDGE_PROOF_new(ctx)))
        return NULL;

    pp = ctx->pp;
    witness = ctx->witness;
    transcript = ctx->transcript;
    group = pp->group;
    order = EC_GROUP_get0_order(group);

    bn_ctx = BN_CTX_new_ex(group->libctx);
    if (bn_ctx == NULL)
        goto err;

    BN_CTX_start(bn_ctx);
    a = BN_CTX_get(bn_ctx);
    e = BN_CTX_get(bn_ctx);
    t = BN_CTX_get(bn_ctx);
    if (t == NULL)
        goto err;

    if (!zkp_rand_range(a, order))
        goto err;

    if (!EC_POINT_mul(group, proof->A, NULL, pp->G, a, bn_ctx))
        goto err;

    if (!ZKP_TRANSCRIPT_append_point(transcript, "G", pp->G, group)
        || !ZKP_TRANSCRIPT_append_point(transcript, "H", pp->H, group)
        || !ZKP_TRANSCRIPT_append_point(transcript, "A", proof->A, group))
        goto err;

    if (!ZKP_TRANSCRIPT_challange(transcript, "e", e))
        goto err;

    if (!BN_mul(t, e, witness->v, bn_ctx)
        || !BN_mod_add(proof->z, a, t, order, bn_ctx))
        goto err;

    ret = proof;
    proof = NULL;
err:
    BN_CTX_end(bn_ctx);
    BN_CTX_free(bn_ctx);
    NIZK_DLOG_KNOWLEDGE_PROOF_free(proof);
    ZKP_TRANSCRIPT_reset(transcript);
    return ret;
}

int NIZK_DLOG_KNOWLEDGE_PROOF_verify(NIZK_DLOG_KNOWLEDGE_CTX *ctx,
                                     NIZK_DLOG_KNOWLEDGE_PROOF *proof)
{
    int ret = 0;
    ZKP_TRANSCRIPT *transcript;
    NIZK_PUB_PARAM *pp;
    EC_GROUP *group;
    BN_CTX *bn_ctx = NULL;
    BIGNUM *e, *bn1, *bn_1;
    EC_POINT *L = NULL, *R = NULL;
    zkp_poly_points_t *poly = NULL;

    if (ctx == NULL || proof == NULL) {
        ERR_raise(ERR_LIB_ZKP_NIZK, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    pp = ctx->pp;
    transcript = ctx->transcript;
    group = pp->group;

    if (!(L = EC_POINT_new(group)) || !(R = EC_POINT_new(group)))
        goto err;

    bn_ctx = BN_CTX_new_ex(group->libctx);
    if (bn_ctx == NULL)
        goto err;

    BN_CTX_start(bn_ctx);
    e = BN_CTX_get(bn_ctx);
    bn1 = BN_CTX_get(bn_ctx);
    bn_1 = BN_CTX_get(bn_ctx);
    if (bn_1 == NULL)
        goto err;

    BN_one(bn1);
    BN_one(bn_1);
    BN_set_negative(bn_1, 1);

    if (!ZKP_TRANSCRIPT_append_point(transcript, "G", pp->G, group)
        || !ZKP_TRANSCRIPT_append_point(transcript, "H", pp->H, group)
        || !ZKP_TRANSCRIPT_append_point(transcript, "A", proof->A, group))
        goto err;

    if (!ZKP_TRANSCRIPT_challange(transcript, "e", e))
        goto err;

    if (!EC_POINT_mul(group, L, NULL, pp->G, proof->z, bn_ctx))
        goto err;

    if (!(poly = zkp_poly_points_new(3)))
        goto err;

    if (!zkp_poly_points_append(poly, proof->A, bn1)
        || !zkp_poly_points_append(poly, pp->H, e)
        || !zkp_poly_points_append(poly, L, bn_1))
        goto err;

    if (!zkp_poly_points_mul(poly, R, NULL, group, bn_ctx))
        goto err;

    if (!EC_POINT_is_at_infinity(group, R))
        goto err;

    ret = 1;
err:
    BN_CTX_end(bn_ctx);
    BN_CTX_free(bn_ctx);
    EC_POINT_free(L);
    EC_POINT_free(R);
    zkp_poly_points_free(poly);
    ZKP_TRANSCRIPT_reset(transcript);
    return ret;
}


